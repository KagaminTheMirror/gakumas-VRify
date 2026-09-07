#ifndef OPENXR_PLATFORM_H_
#define OPENXR_PLATFORM_H_ 1

/*
** Copyright 2017-2026 The Khronos Group Inc.
**
** SPDX-License-Identifier: Apache-2.0 OR MIT
**
** Minimal D3D11 subset extracted from the generated OpenXR 1.1.61 platform
** header. See deps/openxr/README.md.
*/

#include "openxr.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef XR_USE_GRAPHICS_API_D3D11

#define XR_KHR_D3D11_enable 1
#define XR_KHR_D3D11_enable_SPEC_VERSION 11
#define XR_KHR_D3D11_ENABLE_EXTENSION_NAME "XR_KHR_D3D11_enable"

typedef struct XrGraphicsBindingD3D11KHR {
    XrStructureType type;
    const void* XR_MAY_ALIAS next;
    ID3D11Device* device;
} XrGraphicsBindingD3D11KHR;

typedef struct XrSwapchainImageD3D11KHR {
    XrStructureType type;
    void* XR_MAY_ALIAS next;
    ID3D11Texture2D* texture;
} XrSwapchainImageD3D11KHR;

typedef struct XrGraphicsRequirementsD3D11KHR {
    XrStructureType type;
    void* XR_MAY_ALIAS next;
    LUID adapterLuid;
    D3D_FEATURE_LEVEL minFeatureLevel;
} XrGraphicsRequirementsD3D11KHR;

typedef XrResult (XRAPI_PTR *PFN_xrGetD3D11GraphicsRequirementsKHR)(XrInstance instance, XrSystemId systemId, XrGraphicsRequirementsD3D11KHR* graphicsRequirements);

#ifdef __cplusplus
}
#endif

#endif

#endif
