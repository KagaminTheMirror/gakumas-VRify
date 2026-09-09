OpenXrContext::FrameWork* OpenXrContext::WorkFor(std::uint64_t frameId) noexcept {
    const int index = coordinator_.SlotIndex(frameId);
    if (index < 0 || index > 1) {
        return nullptr;
    }
    return &works_[static_cast<std::size_t>(index)];
}

void OpenXrContext::FreezeGpuSubmitSnapshot(FrameWork& work) noexcept {
    GpuSubmitSnapshot& gpu = work.gpuSnapshot;
    gpu.aaMenuVisible = aaMenuVisible_;
    gpu.stereoUiPanelVisible = stereoUiPanelVisible_;
    gpu.panelAdjustMode = panelAdjustMode_;
    gpu.panelToastKind = panelToastKind_;
    gpu.gripHeld = gripHeld_;
    gpu.panelPoseUsesBase = panelPoseUsesBase_;
    gpu.panelPoseView = panelPoseView_;
    gpu.panelPoseBase = panelPoseBase_;
    gpu.barPoseView = barPoseView_;
    gpu.barPoseBase = barPoseBase_;
    gpu.hintPoseView = hintPoseView_;
    gpu.panelQuadWidth = panelQuadWidth_;
    gpu.barQuadWidth = barQuadWidth_;
    gpu.barQuadHeight = barQuadHeight_;
    gpu.panelHintWidthPx = panelHintWidthPx_;
    gpu.panelHintHeightPx = panelHintHeightPx_;
    gpu.mirrorWidth = work.frame.mirrorWidth;
    gpu.mirrorHeight = work.frame.mirrorHeight;
    gpu.mirrorLayoutGeneration = work.frame.mirrorLayoutGeneration;
}

void OpenXrContext::ResetFrameProtocol() noexcept {
    inputProjectionReady_.store(false, std::memory_order_release);
    pendingStereoRenderScaleValid_.store(false, std::memory_order_release);
    for (FrameWork& work : works_) {
        work.stereo.Reset();
        work.identity = {};
        work.frame = {};
        work.gpuSnapshot = {};
        work.sessionLossPending = false;
        work.began = false;
        work.locateResult = XR_SUCCESS;
        work.mirrorResult = XR_SUCCESS;
        work.locateViewCount = 0;
        work.appliedReferenceSpaceChangeTime = 0;
        work.appliedReferenceSpaceChangeCount = 0;
        work.effectiveProjectionTrackingTimeFloor = 0;
        work.stereoBound = false;
        work.projectionReady = false;
        work.mirrorReady = false;
        work.menuReady = false;
        work.overlayReady = false;
        work.projectionTracking = {};
        work.sourceFrame = nullptr;
        work.sourceFrameGeneration = 0;
        work.sourceLayoutGeneration = 0;
        work.endEventId = 0;
        work.layers = {};
    }
    coordinator_.Reset(sessionRunGeneration_);
}

void OpenXrContext::LogFrame(
    VrLog& log,
    std::string_view token,
    const frame::FrameIdentity& identity,
    XrTime displayTime) {
    std::ostringstream line;
    line << "[VR][frame] " << token << " frameId=" << identity.frameId
         << " pred=" << identity.predecessorFrameId
         << " session=" << identity.sessionGeneration
         << " spaceEpoch=" << identity.referenceSpaceEpoch
         << " predictedDisplayTime=" << displayTime;
    log.Write(line.str());
}

frame::FrameCoordinator& OpenXrContext::Coordinator() noexcept {
    return coordinator_;
}

const frame::FrameCoordinator& OpenXrContext::Coordinator() const noexcept {
    return coordinator_;
}

OpenXrContext::FrameResult OpenXrContext::WaitAndPrepare(
    StereoFrame& frame,
    ID3D11Texture2D* sourceFrame,
    std::uint64_t sourceFrameGeneration,
    std::uint64_t sourceLayoutGeneration,
    frame::FrameIdentity& ticket,
    VrLog& log) {
    const bool gripPanelTransparent = frame.gripPanelTransparent;
    frame = StereoFrame{};
    frame.gripPanelTransparent = gripPanelTransparent;
    ticket = {};

    if (session_ == XR_NULL_HANDLE || localSpace_ == XR_NULL_HANDLE) {
        lastResult_ = XR_ERROR_HANDLE_INVALID;
        log.Write("[VR][runtime] frame skipped: session or LOCAL space is unavailable");
        return FrameResult::Failed;
    }
    if (!sessionRunning_) {
        lastResult_ = XR_ERROR_SESSION_NOT_RUNNING;
        return FrameResult::SessionNotRunning;
    }
    if (pendingStereoRenderScaleValid_.exchange(false, std::memory_order_acq_rel)) {
        (void)ApplyStereoRenderScale(
            pendingStereoRenderScale_.load(std::memory_order_acquire), log);
    }
    if (dispatch_.WaitFrame() == nullptr || dispatch_.BeginFrame() == nullptr ||
        dispatch_.LocateViews() == nullptr || dispatch_.EndFrame() == nullptr) {
        lastResult_ = XR_ERROR_FUNCTION_UNSUPPORTED;
        log.Write("[VR][runtime] frame entry points are unavailable");
        return FrameResult::Failed;
    }

    bool mirrorLayoutChanged = false;
    const std::uint64_t renderId = coordinator_.RenderFrameId();
    const bool allowMirrorRebuild =
        renderId == 0 || coordinator_.EndReturned(renderId);
    if (!EnsureMirrorLayout(
            sourceFrame,
            sourceLayoutGeneration,
            allowMirrorRebuild,
            mirrorLayoutChanged,
            log)) {
        return ClassifyFrameResult(lastResult_);
    }
    frame.mirrorLayoutChanged = mirrorLayoutChanged;
    frame.mirrorLayoutGeneration = mirrorLayoutGeneration_;
    frame.mirrorWidth = mirrorWidth_;
    frame.mirrorHeight = mirrorHeight_;

    const frame::TicketError startError = coordinator_.TryStartWait(0, ticket);
    if (startError != frame::TicketError::None) {
        lastResult_ = XR_ERROR_VALIDATION_FAILURE;
        log.Write(
            std::string("[VR][frame] TICKET_REPEAT_REJECTED reason=") +
            frame::TicketErrorName(startError));
        return FrameResult::ProtocolRejected;
    }
    FrameWork* work = WorkFor(ticket.frameId);
    if (work == nullptr) {
        (void)coordinator_.Cancel(ticket.frameId, frame::TicketError::InvalidState);
        return FrameResult::Failed;
    }
    work->stereo.Reset();
    work->identity = ticket;
    work->frame = frame;
    work->sessionLossPending = false;
    work->began = false;
    work->locateResult = XR_SUCCESS;
    work->mirrorResult = XR_SUCCESS;
    work->locateViewCount = 0;
    work->appliedReferenceSpaceChangeTime = 0;
    work->appliedReferenceSpaceChangeCount = 0;
    work->effectiveProjectionTrackingTimeFloor = 0;
    work->stereoBound = false;
    work->projectionReady = false;
    work->mirrorReady = false;
    work->menuReady = false;
    work->overlayReady = false;
    work->projectionTracking = {};
    work->sourceFrame = sourceFrame;
    work->sourceFrameGeneration = sourceFrameGeneration;
    work->sourceLayoutGeneration = sourceLayoutGeneration;
    work->endEventId = 0;
    work->layers = {};
    work->gpuSnapshot = {};

    coordinator_.MarkWaitEntered(ticket.frameId);
    LogFrame(log, "WAIT_ENTERED", ticket, 0);
    XrFrameWaitInfo waitInfo{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState frameState{XR_TYPE_FRAME_STATE};
    static thread_local perf::Accumulator waitTiming;
    const bool timing = GakumasLocal::Config::vrDiagnosticsStartupEnabled;
    const auto timingSink = [&log](std::string_view line) noexcept { log.Write(line); };
    perf::Scope waitScope(waitTiming, timing, "xr.wait-frame", timingSink, sessionState_);
    const XrResult waitResult = dispatch_.WaitFrame()(session_, &waitInfo, &frameState);
    lastResult_ = waitResult;
    waitScope.Stop();
    if (XR_FAILED(waitResult)) {
        log.Write(
            "[VR][runtime] xrWaitFrame failed: " +
            dispatch_.ResultText(instance_, waitResult));
        (void)coordinator_.Cancel(ticket.frameId, frame::TicketError::InvalidState);
        return ClassifyFrameResult(waitResult);
    }
    work->sessionLossPending = waitResult == XR_SESSION_LOSS_PENDING;
    frame.predictedDisplayTime = frameState.predictedDisplayTime;
    frame.predictedDisplayPeriod = frameState.predictedDisplayPeriod;
    frame.shouldRender = frameState.shouldRender == XR_TRUE;

    XrTime appliedReferenceSpaceChangeTime = 0;
    std::size_t appliedReferenceSpaceChangeCount = 0;
    for (const XrTime changeTime : pendingReferenceSpaceChangeTimes_) {
        if (frameState.predictedDisplayTime < changeTime) {
            break;
        }
        appliedReferenceSpaceChangeTime =
            std::max(appliedReferenceSpaceChangeTime, changeTime);
        ++appliedReferenceSpaceChangeCount;
    }
    frame.referenceSpaceChanged = appliedReferenceSpaceChangeCount != 0;
    work->appliedReferenceSpaceChangeTime = appliedReferenceSpaceChangeTime;
    work->appliedReferenceSpaceChangeCount = appliedReferenceSpaceChangeCount;
    work->effectiveProjectionTrackingTimeFloor = std::max(
        projectionTrackingTimeFloor_, appliedReferenceSpaceChangeTime);

    XrViewLocateInfo locateInfo{XR_TYPE_VIEW_LOCATE_INFO};
    locateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    locateInfo.displayTime = frameState.predictedDisplayTime;
    locateInfo.space = stageSpace_ != XR_NULL_HANDLE ? stageSpace_ : localSpace_;
    XrViewState viewState{XR_TYPE_VIEW_STATE};
    std::array<XrView, 2> views{
        XrView{XR_TYPE_VIEW},
        XrView{XR_TYPE_VIEW},
    };
    uint32_t viewCount = 0;
    work->locateResult = dispatch_.LocateViews()(
        session_,
        &locateInfo,
        &viewState,
        static_cast<uint32_t>(views.size()),
        &viewCount,
        views.data());
    work->locateViewCount = viewCount;
    pose::StereoPoseSample tracking{};
    if (XR_SUCCEEDED(work->locateResult)) {
        work->sessionLossPending =
            work->sessionLossPending || work->locateResult == XR_SESSION_LOSS_PENDING;
        frame.viewStateFlags = viewState.viewStateFlags;
        frame.viewCount = std::min(viewCount, static_cast<uint32_t>(views.size()));
        tracking.valid = frame.viewCount == 2;
        tracking.sessionGeneration = sessionRunGeneration_;
        tracking.referenceSpaceType =
            static_cast<std::int32_t>(ActiveReferenceSpaceType());
        tracking.predictedDisplayTime = frameState.predictedDisplayTime;
        tracking.viewStateFlags = viewState.viewStateFlags;
        tracking.viewCount = frame.viewCount;
        for (uint32_t index = 0; index < frame.viewCount; ++index) {
            frame.views[index].pose = views[index].pose;
            frame.views[index].fov = views[index].fov;
            tracking.eyes[index].pose = ToTrackingPose(views[index].pose);
            tracking.eyes[index].fov = {
                views[index].fov.angleLeft,
                views[index].fov.angleRight,
                views[index].fov.angleUp,
                views[index].fov.angleDown,
            };
        }
    } else {
        log.Write(
            "[VR][runtime] xrLocateViews failed: " +
            dispatch_.ResultText(instance_, work->locateResult));
    }

    UpdatePanelPlacementFrame(frameState.predictedDisplayTime, log);
    SyncPointerInput(frame.pointers, frameState.predictedDisplayTime, log);
    // PublishPreparedOutputs dispatches game input immediately after this
    // phase. Resolve Grip/menu ownership now, not at the render-tail Submit.
    // The previous submitted role describes the panel currently on display.
    const bool inputStereoEligible = frame.shouldRender &&
        stereoProjectionEnabled_ &&
        !projectionDisabledForSession_.load(std::memory_order_acquire) &&
        (!stereoLandscapeOnly_ || mirrorWidth_ >= mirrorHeight_);
    UpdateStereoUiPanelState(
        frame.pointers, inputStereoEligible,
        inputStereoEligible &&
            inputProjectionReady_.load(std::memory_order_acquire),
        frame.shouldRender && mirrorSwapchain_ != XR_NULL_HANDLE,
        frame.predictedDisplayTime, frame, log);
    work->stereoBound = false;

    (void)coordinator_.SetEpochs(
        ticket.frameId,
        referenceSpaceEpoch_,
        (static_cast<std::uint64_t>(stereoEyeWidth_) << 32) |
            stereoEyeHeight_,
        mirrorLayoutGeneration_);
    frame::FrameSnapshot snap{};
    if (coordinator_.CopySnapshot(ticket.frameId, snap)) {
        ticket = snap.identity;
    }
    frame::FrameTiming timingState{};
    timingState.predictedDisplayTime = frameState.predictedDisplayTime;
    timingState.predictedDisplayPeriod = frameState.predictedDisplayPeriod;
    timingState.shouldRender = frame.shouldRender;
    if (coordinator_.CompleteWait(ticket.frameId, timingState, tracking) !=
            frame::TicketError::None ||
        coordinator_.MarkCpuReady(ticket.frameId) != frame::TicketError::None) {
        (void)coordinator_.Cancel(ticket.frameId, frame::TicketError::InvalidState);
        return FrameResult::ProtocolRejected;
    }
    work->frame = frame;
    FreezeGpuSubmitSnapshot(*work);
    LogFrame(log, "WAIT_RETURNED", ticket, frameState.predictedDisplayTime);
    log.Write("[VR][frame] CPU_READY frameId=" + std::to_string(ticket.frameId));
    return FrameResult::Completed;
}

OpenXrContext::FrameResult OpenXrContext::BeginPrepared(
    const frame::FrameIdentity& ticket,
    StereoFrame& frame,
    VrLog& log) {
    FrameWork* work = WorkFor(ticket.frameId);
    if (work == nullptr) {
        return FrameResult::ProtocolRejected;
    }
    frame = work->frame;
    const frame::TicketError beginError = coordinator_.TryBegin(ticket.frameId);
    if (beginError == frame::TicketError::PredecessorNotReturned) {
        log.Write(
            "[VR][frame] GRAPHICS_GATE closed frameId=" +
            std::to_string(ticket.frameId));
        return FrameResult::GraphicsGateClosed;
    }
    if (beginError != frame::TicketError::None) {
        log.Write(
            std::string("[VR][frame] TICKET_REPEAT_REJECTED begin=") +
            frame::TicketErrorName(beginError) +
            " frameId=" + std::to_string(ticket.frameId));
        return FrameResult::ProtocolRejected;
    }
    LogFrame(log, "GRAPHICS_GATE_PASSED", ticket, frame.predictedDisplayTime);
    LogFrame(log, "BEGIN_ENTERED", ticket, frame.predictedDisplayTime);

    XrFrameBeginInfo beginInfo{XR_TYPE_FRAME_BEGIN_INFO};
    static thread_local perf::Accumulator beginTiming;
    const bool timing = GakumasLocal::Config::vrDiagnosticsStartupEnabled;
    const auto timingSink = [&log](std::string_view line) noexcept { log.Write(line); };
    perf::Scope beginScope(beginTiming, timing, "xr.begin-frame", timingSink, sessionState_);
    lastResult_ = dispatch_.BeginFrame()(session_, &beginInfo);
    beginScope.Stop();
    if (XR_FAILED(lastResult_)) {
        log.Write(
            "[VR][runtime] xrBeginFrame failed: " +
            dispatch_.ResultText(instance_, lastResult_));
        (void)coordinator_.Cancel(ticket.frameId, frame::TicketError::InvalidState);
        return ClassifyFrameResult(lastResult_);
    }
    work->began = true;
    work->sessionLossPending =
        work->sessionLossPending || lastResult_ == XR_SESSION_LOSS_PENDING;
    frame.frameDiscarded = lastResult_ == XR_FRAME_DISCARDED;
    (void)coordinator_.MarkRendering(ticket.frameId);
    work->frame = frame;
    return FrameResult::Completed;
}

OpenXrContext::FrameResult OpenXrContext::SubmitPrepared(
    const frame::FrameIdentity& ticket,
    StereoFrame& frame,
    ID3D11Texture2D* sourceFrame,
    std::uint64_t sourceFrameGeneration,
    const d3d11::StereoRenderMailbox* stereoMailbox,
    VrLog& log) {
    FrameWork* work = WorkFor(ticket.frameId);
    if (work == nullptr || !work->began) {
        return FrameResult::ProtocolRejected;
    }
    frame = work->frame;
    const GpuSubmitSnapshot& gpu = work->gpuSnapshot;
    const bool waitMirrorInput = frame.mirrorInputEnabled;
    if (coordinator_.MarkEndDispatched(ticket.frameId) != frame::TicketError::None) {
        return FrameResult::ProtocolRejected;
    }
    if (sourceFrame != nullptr) {
        work->sourceFrame = sourceFrame;
        work->sourceFrameGeneration = sourceFrameGeneration;
    }
    if (stereoMailbox != nullptr && !work->stereoBound &&
        stereoMailbox->ConsumeOnce(work->stereo)) {
        work->stereoBound = true;
        (void)coordinator_.BindStereo(
            ticket.frameId, work->stereo.generation, work->stereo.trackingSample);
        LogFrame(log, "TICKET_CONSUME", ticket, frame.predictedDisplayTime);
        std::ostringstream consume;
        consume << "[VR][frame] TICKET_CONSUME frameId=" << ticket.frameId
                << " generation=" << work->stereo.generation
                << " predictedDisplayTime=" << frame.predictedDisplayTime;
        log.Write(consume.str());
    } else if (!work->stereoBound) {
        std::ostringstream rejected;
        rejected << "[VR][frame] TICKET_REPEAT_REJECTED frameId=" << ticket.frameId
                 << " predictedDisplayTime=" << frame.predictedDisplayTime;
        log.Write(rejected.str());
    }
    frame::GraphicsCounters graphics{};
    graphics.beginEntered = 1;

    bool projectionReady = false;
    bool mirrorReady = false;
    bool menuReady = false;
    bool overlayReady = false;
    XrResult mirrorResult = XR_SUCCESS;
    const d3d11::StereoRenderMailbox::Snapshot* stereoFrame =
        work->stereoBound ? &work->stereo : nullptr;
    if (frame.shouldRender) {
        const bool stereoFrameFresh = stereoFrame != nullptr &&
            !frame.referenceSpaceChanged &&
            stereoFrame->IsComplete() &&
            stereoFrame->trackingSample.sessionGeneration == sessionRunGeneration_ &&
            stereoFrame->trackingSample.referenceSpaceType ==
                static_cast<std::int32_t>(ActiveReferenceSpaceType()) &&
            stereoFrame->trackingSample.predictedDisplayTime >=
                work->effectiveProjectionTrackingTimeFloor;
        const bool stereoSceneEligible = stereoProjectionEnabled_ &&
            !projectionDisabledForSession_.load(std::memory_order_acquire) &&
            (!stereoLandscapeOnly_ || gpu.mirrorWidth >= gpu.mirrorHeight);
        if (stereoSceneEligible && stereoFrameFresh) {
            projectionReady = RenderProjectionFrame(*stereoFrame, log);
            if (!projectionReady) {
                log.Write(
                    "[VR][stereo] PROJECTION_DISABLED_FOR_SESSION result=" +
                    dispatch_.ResultText(instance_, lastResult_));
                projectionDisabledForSession_.store(true, std::memory_order_release);
                ResetProjectionSwapchain();
            } else {
                work->projectionTracking = stereoFrame->trackingSample;
                lastSubmittedStereoGeneration_ = stereoFrame->generation;
                lastSubmittedStereoTrackingSample_ = stereoFrame->trackingSample;
                lastSubmittedStereoHostPublishTimeNanoseconds_ =
                    stereoFrame->hostPublishTimeNanoseconds;
                frame.stereoFrameGeneration = stereoFrame->generation;
                ++stereoSubmissionCount_;
                ++freshStereoSubmissionCount_;
                graphics.copy += 1;
                if (stereoSubmissionCount_ <= 4U ||
                    stereoSubmissionCount_ % 300U == 0U) {
                    std::ostringstream projectionTiming;
                    projectionTiming << "[VR][stereo] PROJECTION_TIMING samples="
                           << stereoSubmissionCount_
                           << " frameId=" << ticket.frameId
                           << " generation=" << stereoFrame->generation
                           << " new=1 copied=1"
                           << " lease=1"
                           << " fresh=" << freshStereoSubmissionCount_
                           << " repeated=0"
                           << " poseRevision="
                           << stereoFrame->trackingSample.revision
                           << " predictedDisplayTime="
                           << stereoFrame->trackingSample.predictedDisplayTime
                           << " submitDisplayTime="
                           << frame.predictedDisplayTime
                           << " displayDeltaMs="
                           << static_cast<double>(
                                  frame.predictedDisplayTime -
                                  stereoFrame->trackingSample.predictedDisplayTime) /
                                  1'000'000.0;
                    log.Write(projectionTiming.str());
                }
            }
        } else if (stereoSceneEligible) {
            std::ostringstream fallback;
            fallback << "[VR][stereo] PROJECTION_FALLBACK frameId=" << ticket.frameId
                     << " generation=0 reason=no-fresh-eyes predictedDisplayTime="
                     << frame.predictedDisplayTime;
            log.Write(fallback.str());
        }
        bool gripNeedsMirrorRefresh = false;
        for (std::size_t hand = 0; hand < frame.pointers.size(); ++hand) {
            const auto& pointer = frame.pointers[hand];
            gripNeedsMirrorRefresh = gripNeedsMirrorRefresh || gpu.gripHeld[hand] ||
                (pointer.gripActive &&
                 pointer.gripValue >= kGripPressThreshold);
        }
        const bool mirrorRefreshRequired =
            !projectionReady || gpu.stereoUiPanelVisible ||
            gripNeedsMirrorRefresh || gpu.panelAdjustMode;
        if (mirrorRefreshRequired) {
            if (mirrorCopySuspended_) {
                mirrorCopySuspended_ = false;
                log.Write("[VR][display] MIRROR_COPY_RESUMED reason=grip-or-panel-or-fallback");
            }
            mirrorReady = RenderMirrorFrame(
                sourceFrame, sourceFrameGeneration, frame.pointers, log);
            mirrorResult = lastResult_;
            if (mirrorReady) {
                frame.sourceFrameGeneration = sourceFrameGeneration;
                graphics.copy += 1;
            }
        } else if (!mirrorCopySuspended_) {
            mirrorCopySuspended_ = true;
            log.Write("[VR][display] MIRROR_COPY_SUSPENDED reason=stereo-panel-hidden");
        }
        if (!mirrorReady) {
            frame.stereoUiPanelVisible = false;
            frame.mirrorInputEnabled = false;
        } else {
            frame.stereoUiPanelVisible =
                projectionReady && gpu.stereoUiPanelVisible && !gpu.aaMenuVisible;
            frame.mirrorInputEnabled = !gpu.aaMenuVisible && !gpu.panelAdjustMode &&
                (!projectionReady || frame.stereoUiPanelVisible);
        }
        if (gpu.aaMenuVisible) {
            menuReady = RenderMenuFrame(frame.pointers, log);
            if (menuReady) {
                graphics.uiGpu += 1;
            }
        }
        if (gpu.panelAdjustMode || gpu.panelToastKind != 0) {
            overlayReady = RenderPanelOverlayFrame(frame.pointers, log);
            if (overlayReady) {
                graphics.uiGpu += 1;
            }
        }
    } else {
        frame.aaMenuVisible = gpu.aaMenuVisible;
        frame.stereoUiPanelVisible = false;
        frame.mirrorInputEnabled = false;
    }
    inputProjectionReady_.store(projectionReady, std::memory_order_release);
    frame.aaMenuVisible = menuReady;
    if (frame.mirrorInputEnabled != waitMirrorInput) {
        frame.mirrorPresentationChanged = true;
    }
    lastMirrorInputEnabled_.store(frame.mirrorInputEnabled, std::memory_order_release);
    mirrorInputStateInitialized_.store(true, std::memory_order_release);
    work->projectionReady = projectionReady;
    work->mirrorReady = mirrorReady;
    work->menuReady = menuReady;
    work->overlayReady = overlayReady;
    work->mirrorResult = mirrorResult;
    (void)coordinator_.RecordGraphics(ticket.frameId, graphics);
    (void)coordinator_.RecordMirrorAvailable(ticket.frameId, mirrorReady);
    if (projectionReady) {
        (void)coordinator_.RecordEyeComplete(ticket.frameId, true, true);
    }

    SubmitLayers& layers = work->layers;
    layers = {};
    layers.displayTime = frame.predictedDisplayTime;
    layers.blendMode = environmentBlendMode_;
    XrSpace panelSpace = gpu.panelPoseUsesBase
        ? (stageSpace_ != XR_NULL_HANDLE ? stageSpace_ : localSpace_)
        : viewSpace_;
    layers.mirror.space = panelSpace;
    layers.mirror.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    layers.mirror.subImage.swapchain = mirrorSwapchain_;
    layers.mirror.subImage.imageRect.extent.width =
        static_cast<int32_t>(gpu.mirrorWidth);
    layers.mirror.subImage.imageRect.extent.height =
        static_cast<int32_t>(gpu.mirrorHeight);
    layers.mirror.pose = ToXrPose(
        gpu.panelPoseUsesBase ? gpu.panelPoseBase : gpu.panelPoseView);
    layers.mirror.size.width = gpu.panelQuadWidth;
    layers.mirror.size.height = gpu.mirrorWidth == 0
        ? 0.0F
        : layers.mirror.size.width *
            static_cast<float>(gpu.mirrorHeight) /
            static_cast<float>(gpu.mirrorWidth);
    if (frame.gripPanelTransparent && projectionReady) {
        layers.mirror.layerFlags |= XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    }
    layers.bar.space = panelSpace;
    layers.bar.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    layers.bar.subImage.swapchain = panelOverlaySwapchain_;
    layers.bar.subImage.imageRect.offset = {0, 0};
    layers.bar.subImage.imageRect.extent.width =
        static_cast<int32_t>(kPanelOverlayTextureWidth);
    layers.bar.subImage.imageRect.extent.height =
        static_cast<int32_t>(kPanelOverlayBarHeight);
    layers.bar.pose = ToXrPose(
        gpu.panelPoseUsesBase ? gpu.barPoseBase : gpu.barPoseView);
    layers.bar.size.width = gpu.barQuadWidth;
    layers.bar.size.height = gpu.barQuadHeight;
    constexpr float kHintMetresPerPixel =
        kHintQuadWidthMetres / static_cast<float>(kPanelOverlayTextureWidth);
    const std::uint32_t hintWidthPx =
        std::min(gpu.panelHintWidthPx, kPanelOverlayTextureWidth);
    const std::uint32_t hintHeightPx =
        std::min(gpu.panelHintHeightPx, kPanelOverlayHintHeight);
    layers.hint.space = viewSpace_;
    layers.hint.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    layers.hint.subImage.swapchain = panelOverlaySwapchain_;
    layers.hint.subImage.imageRect.offset = {
        static_cast<int32_t>((kPanelOverlayTextureWidth - hintWidthPx) / 2U),
        static_cast<int32_t>(kPanelOverlayHintTop)};
    layers.hint.subImage.imageRect.extent.width = static_cast<int32_t>(hintWidthPx);
    layers.hint.subImage.imageRect.extent.height = static_cast<int32_t>(hintHeightPx);
    layers.hint.pose = ToXrPose(gpu.hintPoseView);
    layers.hint.size.width = static_cast<float>(hintWidthPx) * kHintMetresPerPixel;
    layers.hint.size.height = static_cast<float>(hintHeightPx) * kHintMetresPerPixel;
    layers.menu.space = viewSpace_;
    layers.menu.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    layers.menu.subImage.swapchain = menuSwapchain_;
    layers.menu.subImage.imageRect.extent.width = static_cast<int32_t>(menuWidth_);
    layers.menu.subImage.imageRect.extent.height = static_cast<int32_t>(menuHeight_);
    layers.menu.pose.orientation.w = 1.0F;
    layers.menu.pose.position.z = kMenuPlaneZ;
    layers.menu.size.width = kMenuWidthMetres;
    layers.menu.size.height = kMenuHeightMetres;
    layers.projection.space = stageSpace_ != XR_NULL_HANDLE ? stageSpace_ : localSpace_;
    layers.projection.viewCount = static_cast<uint32_t>(layers.projectionViews.size());
    layers.projection.views = layers.projectionViews.data();
    if (projectionReady) {
        for (std::size_t eyeIndex = 0; eyeIndex < layers.projectionViews.size(); ++eyeIndex) {
            const auto& trackedEye = work->projectionTracking.eyes[eyeIndex];
            layers.projectionViews[eyeIndex].pose.position = {
                trackedEye.pose.position.x,
                trackedEye.pose.position.y,
                trackedEye.pose.position.z,
            };
            layers.projectionViews[eyeIndex].pose.orientation = {
                trackedEye.pose.orientation.x,
                trackedEye.pose.orientation.y,
                trackedEye.pose.orientation.z,
                trackedEye.pose.orientation.w,
            };
            layers.projectionViews[eyeIndex].fov = {
                trackedEye.fov.angleLeft,
                trackedEye.fov.angleRight,
                trackedEye.fov.angleUp,
                trackedEye.fov.angleDown,
            };
            layers.projectionViews[eyeIndex].subImage.swapchain = projectionSwapchain_;
            layers.projectionViews[eyeIndex].subImage.imageRect.extent.width =
                static_cast<int32_t>(projectionSourceDescription_.Width);
            layers.projectionViews[eyeIndex].subImage.imageRect.extent.height =
                static_cast<int32_t>(projectionSourceDescription_.Height);
            layers.projectionViews[eyeIndex].subImage.imageArrayIndex =
                static_cast<uint32_t>(eyeIndex);
        }
    }
    layers.layerCount = 0;
    if (projectionReady) {
        layers.layerPtrs[layers.layerCount++] =
            reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layers.projection);
    }
    const bool mirrorLayerVisible = mirrorReady && !frame.aaMenuVisible &&
        (!projectionReady || frame.stereoUiPanelVisible);
    if (mirrorLayerVisible) {
        layers.layerPtrs[layers.layerCount++] =
            reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layers.mirror);
    }
    if (overlayReady && gpu.panelAdjustMode && mirrorLayerVisible) {
        layers.layerPtrs[layers.layerCount++] =
            reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layers.bar);
    }
    if (overlayReady && hintWidthPx != 0 && hintHeightPx != 0 &&
        (gpu.panelAdjustMode || gpu.panelToastKind != 0)) {
        layers.layerPtrs[layers.layerCount++] =
            reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layers.hint);
    }
    if (menuReady) {
        layers.layerPtrs[layers.layerCount++] =
            reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layers.menu);
    }
    layers.projectionReady = projectionReady;
    layers.mirrorReady = mirrorReady;
    layers.valid = true;
    frame.layerSubmitted = layers.layerCount != 0;
    frame.stereoLayerSubmitted = projectionReady;
    work->frame = frame;
    if (coordinator_.MarkEndQueued(ticket.frameId) != frame::TicketError::None) {
        return FrameResult::ProtocolRejected;
    }
    LogFrame(log, "END_QUEUED", ticket, frame.predictedDisplayTime);
    return FrameResult::Completed;
}

OpenXrContext::FrameResult OpenXrContext::BeginAndSubmit(
    const frame::FrameIdentity& ticket,
    StereoFrame& frame,
    ID3D11Texture2D* sourceFrame,
    std::uint64_t sourceFrameGeneration,
    const d3d11::StereoRenderMailbox* stereoMailbox,
    VrLog& log) {
    const FrameResult begun = BeginPrepared(ticket, frame, log);
    if (begun != FrameResult::Completed) {
        return begun;
    }
    return SubmitPrepared(
        ticket, frame, sourceFrame, sourceFrameGeneration, stereoMailbox, log);
}

OpenXrContext::FrameResult OpenXrContext::EndPrepared(
    const frame::FrameIdentity& ticket,
    StereoFrame& frame,
    VrLog& log) {
    FrameWork* work = WorkFor(ticket.frameId);
    if (work == nullptr || !work->layers.valid) {
        return FrameResult::ProtocolRejected;
    }
    frame = work->frame;
    if (coordinator_.TryEnterEnd(ticket.frameId) != frame::TicketError::None) {
        log.Write(
            "[VR][frame] TICKET_REPEAT_REJECTED frameId=" +
            std::to_string(ticket.frameId) + " reason=end-ownership");
        return FrameResult::ProtocolRejected;
    }
    LogFrame(log, "END_ENTERED", ticket, work->layers.displayTime);

    XrFrameEndInfo endInfo{XR_TYPE_FRAME_END_INFO};
    endInfo.displayTime = work->layers.displayTime;
    endInfo.environmentBlendMode = work->layers.blendMode;
    endInfo.layerCount = work->layers.layerCount;
    endInfo.layers = work->layers.layerCount != 0 ? work->layers.layerPtrs.data() : nullptr;
    static thread_local perf::Accumulator endTiming;
    const bool timing = GakumasLocal::Config::vrDiagnosticsStartupEnabled;
    const auto timingSink = [&log](std::string_view line) noexcept { log.Write(line); };
    perf::Scope endScope(endTiming, timing, "xr.end-frame", timingSink,
        sessionState_, endInfo.layerCount);
    const XrResult endResult = dispatch_.EndFrame()(session_, &endInfo);
    endScope.Stop();
    const bool success = XR_SUCCEEDED(endResult);
    (void)coordinator_.CompleteEnd(ticket.frameId, success);
    LogFrame(log, "END_RETURNED", ticket, work->layers.displayTime);
    if (XR_FAILED(endResult)) {
        lastResult_ = endResult;
        log.Write(
            "[VR][runtime] xrEndFrame failed: " +
            dispatch_.ResultText(instance_, endResult));
        return ClassifyFrameResult(endResult);
    }
    work->sessionLossPending =
        work->sessionLossPending || endResult == XR_SESSION_LOSS_PENDING;

    if (frame.referenceSpaceChanged) {
        projectionTrackingTimeFloor_ = work->effectiveProjectionTrackingTimeFloor;
        pendingReferenceSpaceChangeTimes_.erase(
            pendingReferenceSpaceChangeTimes_.begin(),
            pendingReferenceSpaceChangeTimes_.begin() +
                static_cast<std::ptrdiff_t>(work->appliedReferenceSpaceChangeCount));
        ++referenceSpaceEpoch_;
        log.Write(
            std::string("[VR][pose] REFERENCE_SPACE_CHANGE_APPLIED active=") +
            ReferenceSpaceText(ActiveReferenceSpaceType()) +
            " changeTime=" + std::to_string(work->appliedReferenceSpaceChangeTime) +
            " predictedDisplayTime=" + std::to_string(frame.predictedDisplayTime) +
            " coalesced=" + std::to_string(work->appliedReferenceSpaceChangeCount));
    }

    const bool projectionReady = work->projectionReady;
    const bool mirrorReady = work->mirrorReady;
    const XrResult locateResult = work->locateResult;
    const std::uint32_t viewCount = work->locateViewCount;
    const bool sessionLossPending = work->sessionLossPending;
    work->stereo.Reset();
    if (!mirrorReady && !projectionReady && frame.shouldRender) {
        // xrEndFrame already succeeded. Zero layers are legal while Live
        // load parks the eyes and the mirror swapchain still has the
        // previous layout. Do not classify the copy miss as a session fault.
        log.Write(
            "[VR][display] FRAME_LAYERS_EMPTY reason=no-projection-or-mirror result=" +
            dispatch_.ResultText(instance_, work->mirrorResult));
    }
    if (XR_FAILED(locateResult)) {
        lastResult_ = locateResult;
        return ClassifyFrameResult(locateResult);
    }
    if (viewCount != 2) {
        lastResult_ = XR_ERROR_RUNTIME_FAILURE;
        log.Write(
            "[VR][runtime] xrLocateViews returned " + std::to_string(viewCount) +
            " view(s); PRIMARY_STEREO requires exactly 2");
        return FrameResult::Failed;
    }
    if (sessionLossPending) {
        lastResult_ = XR_SESSION_LOSS_PENDING;
        return FrameResult::SessionLossPending;
    }
    lastResult_ = XR_SUCCESS;
    return FrameResult::Completed;
}

OpenXrContext::FrameResult OpenXrContext::RunFrame(
    StereoFrame& frame,
    ID3D11Texture2D* sourceFrame,
    std::uint64_t sourceFrameGeneration,
    std::uint64_t sourceLayoutGeneration,
    const d3d11::StereoRenderMailbox* stereoMailbox,
    VrLog& log) {
    static thread_local perf::Accumulator frameTiming;
    const bool timing = GakumasLocal::Config::vrDiagnosticsStartupEnabled;
    const auto timingSink = [&log](std::string_view line) noexcept { log.Write(line); };
    perf::Scope frameScope(frameTiming, timing, "xr.frame", timingSink, sessionState_);
    frame::FrameIdentity ticket{};
    const FrameResult prepared = WaitAndPrepare(
        frame,
        sourceFrame,
        sourceFrameGeneration,
        sourceLayoutGeneration,
        ticket,
        log);
    if (prepared != FrameResult::Completed) {
        return prepared;
    }
    const frame::TicketError gate = coordinator_.WaitForGraphicsGate(ticket.frameId);
    if (gate != frame::TicketError::None) {
        (void)coordinator_.Cancel(ticket.frameId, gate);
        return FrameResult::GraphicsGateClosed;
    }
    const FrameResult submitted = BeginAndSubmit(
        ticket, frame, sourceFrame, sourceFrameGeneration, stereoMailbox, log);
    FrameWork* work = WorkFor(ticket.frameId);
    if (submitted != FrameResult::Completed) {
        if (work != nullptr && work->began) {
            return EndPrepared(ticket, frame, log);
        }
        return submitted;
    }
    return EndPrepared(ticket, frame, log);
}
