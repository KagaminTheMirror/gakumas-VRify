#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <d3d11.h>

#define XR_NO_PROTOTYPES
#define XR_USE_GRAPHICS_API_D3D11
#include "../../../deps/openxr/include/openxr/openxr.h"
#include "../../../deps/openxr/include/openxr/openxr_platform.h"

#if defined(_WIN64)
static_assert(sizeof(XrApiLayerProperties) == 544);
static_assert(sizeof(XrExtensionProperties) == 152);
static_assert(sizeof(XrApplicationInfo) == 272);
static_assert(sizeof(XrInstanceCreateInfo) == 328);
static_assert(sizeof(XrInstanceProperties) == 152);
static_assert(sizeof(XrEventDataBuffer) == 4016);
static_assert(sizeof(XrEventDataEventsLost) == 24);
static_assert(sizeof(XrEventDataInstanceLossPending) == 24);
static_assert(sizeof(XrEventDataSessionStateChanged) == 40);
static_assert(sizeof(XrEventDataReferenceSpaceChangePending) == 72);
static_assert(sizeof(XrSystemGetInfo) == 24);
static_assert(sizeof(XrSystemProperties) == 304);
static_assert(sizeof(XrSessionCreateInfo) == 32);
static_assert(sizeof(XrSpace) == 8);
static_assert(sizeof(XrDuration) == 8);
static_assert(sizeof(XrViewConfigurationType) == 4);
static_assert(sizeof(XrEnvironmentBlendMode) == 4);
static_assert(sizeof(XrReferenceSpaceType) == 4);
static_assert(sizeof(XrCompositionLayerFlags) == 8);
static_assert(sizeof(XrViewStateFlags) == 8);
static_assert(sizeof(XrVector3f) == 12);
static_assert(sizeof(XrQuaternionf) == 16);
static_assert(sizeof(XrPosef) == 28);
static_assert(sizeof(XrReferenceSpaceCreateInfo) == 48);
static_assert(sizeof(XrViewConfigurationView) == 40);
static_assert(sizeof(XrSwapchain) == 8);
static_assert(sizeof(XrSwapchainCreateInfo) == 64);
static_assert(sizeof(XrSwapchainImageBaseHeader) == 16);
static_assert(sizeof(XrSwapchainImageAcquireInfo) == 16);
static_assert(sizeof(XrSwapchainImageWaitInfo) == 24);
static_assert(sizeof(XrSwapchainImageReleaseInfo) == 16);
static_assert(sizeof(XrSessionBeginInfo) == 24);
static_assert(sizeof(XrFrameWaitInfo) == 16);
static_assert(sizeof(XrFrameState) == 40);
static_assert(sizeof(XrFrameBeginInfo) == 16);
static_assert(sizeof(XrCompositionLayerBaseHeader) == 32);
static_assert(sizeof(XrFrameEndInfo) == 40);
static_assert(sizeof(XrViewLocateInfo) == 40);
static_assert(sizeof(XrViewState) == 24);
static_assert(sizeof(XrFovf) == 16);
static_assert(sizeof(XrOffset2Di) == 8);
static_assert(sizeof(XrExtent2Di) == 8);
static_assert(sizeof(XrRect2Di) == 16);
static_assert(sizeof(XrExtent2Df) == 8);
static_assert(sizeof(XrSwapchainSubImage) == 32);
static_assert(sizeof(XrCompositionLayerQuad) == 112);
static_assert(sizeof(XrView) == 64);
static_assert(sizeof(XrGraphicsBindingD3D11KHR) == 24);
static_assert(sizeof(XrSwapchainImageD3D11KHR) == 24);
static_assert(sizeof(XrGraphicsRequirementsD3D11KHR) == 32);
#endif
