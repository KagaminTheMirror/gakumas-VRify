#pragma once
#include <string>
#include <filesystem>
#include "vr/config/VrifyConfig.hpp"

// Initializes the process-wide hook owner. The return value reports whether
// MinHook is ready for Localify and VR registrars.
bool initHook();

void unInitHook();

void loadConfig(const std::string& configJson,
    GakumasLocal::Config::ConfigLoadPurpose purpose =
        GakumasLocal::Config::ConfigLoadPurpose::LocalifyReload);

void loadConfig(const std::filesystem::path& filePath,
    GakumasLocal::Config::ConfigLoadPurpose purpose =
        GakumasLocal::Config::ConfigLoadPurpose::LocalifyReload);

bool getCurrentLodingProgress(int* stepTotal, int* stepCurrent, int* currTotal, int* currCurrent);

// TODO keyboard events

namespace GakumasLocal::WinHooks {
	void* LoadAssetBundle(const std::string& path);

	namespace Keyboard {
		void InstallWndProcHook();
	}
}
