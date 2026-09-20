#pragma once
#include "GakumasLocalify/Hook.h"
#include "GakumasLocalify/HookTexture.h"
#include "GakumasLocalify/Plugin.h"
#include "GakumasLocalify/Log.h"
#include "deps/UnityResolve/UnityResolve.hpp"
#include "GakumasLocalify/Il2cppUtils.hpp"
#include "GakumasLocalify/Local.h"
#include "GakumasLocalify/MasterLocal.h"
#include <array>
#include <unordered_set>
#include <unordered_map>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <ctime>
#include <cstring>
#include "GakumasLocalify/camera/camera.hpp"
#include "vr/config/VrifyConfig.hpp"
#include "vr/PerformanceTiming.hpp"
#include "vr/PerformanceProbe.hpp"
// #include <jni.h>
#include <thread>
#include <map>
#include <set>
#include <list>
#include <limits>
#include <optional>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <vector>
#include <string_view>
#include <initializer_list>
#include <utility>
#include <cctype>
#include <intrin.h>
#include "host/localify/PlatformDefine.hpp"

#ifdef GKMS_WINDOWS
    #include "host/VrWindowsPlatform.hpp"
    #include "vr/VrRuntime.hpp"
    #include "vr/VrFreeCamera.hpp"
    #include "vr/LivePause.hpp"
    #include "vr/VrPhotoShutter.hpp"
    #include "vr/SkyRenderHooks.hpp"
    #include "vr/UnityStereoRenderer.hpp"
    #include "vr/frame/FrameLoopDriver.hpp"
    #include "vr/GripBlurSource.hpp"
    #include "vr/VrHandGlowSticks.hpp"
    #include "vr/input/UnityAnalogScroll.hpp"
    #include "vr/input/UnityPointerInput.hpp"
    #include "vr/pose/RelativePoseBridge.hpp"
    #include "cpprest/details/http_helpers.h"
    #include "resourceUpdate/resourceUpdate.hpp"
#endif



#include "vr/camera/FollowActorSelection.hpp"
