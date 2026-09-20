#pragma once
#include <vector>

namespace gakumas::vr::panorama {
// This game's IL2CPP GC handle ABI is pointer-sized, including get/free arguments.
using GcHandle = void*;
using NewHandle = GcHandle (*)(void*, bool);
using GetTarget = void* (*)(GcHandle);
using FreeHandle = void (*)(GcHandle);
using HandleList = std::vector<GcHandle>;
}
