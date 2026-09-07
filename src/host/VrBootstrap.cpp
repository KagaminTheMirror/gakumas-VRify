#include <stdinclude.hpp>

#include <minizip/unzip.h>
#include <TlHelp32.h>

#include <unordered_set>
#include <charconv>
#include <cassert>
#include <format>
#include <cpprest/uri.h>
#include <cpprest/http_listener.h>
#include <ranges>
#include <windows.h>
#include "host/VrWindowsPlatform.hpp"
#include "vr/config/VrifyConfig.hpp"
#include "hooks/HookManager.hpp"
#include "vr/VrLog.hpp"
#include "vr/VrRuntime.hpp"

extern void start_console();

const auto CONSOLE_TITLE = L"Gakumas Localify";

std::filesystem::path gakumasLocalPath = "./gakumas-local";
std::filesystem::path ProgramConfigJson = gakumasLocalPath / "config.json";
std::filesystem::path ConfigJson = gakumasLocalPath / "localizationConfig.json";
std::filesystem::path VrConfigJson = "./gakumas-vr/config.json";

bool g_has_config_file = false;
bool g_enable_console = false;
bool g_useRemoteAssets = false;
bool g_useAPIAssets = false;
bool g_useAPITextureAssets = false;
bool g_delTextureRemoteAfterUpdate = true;
std::string g_remoteResourceUrl = "";
std::string g_useAPIAssetsURL = "";
std::string g_useAPITextureAssetsURL = "https://texture.gakumas.cn/api/gkms_texture_data";

namespace
{
	void create_debug_console()
	{
		AllocConsole();

		// open stdout stream
		auto _ = freopen("CONOUT$", "w+t", stdout);
		_ = freopen("CONOUT$", "w", stderr);
		_ = freopen("CONIN$", "r", stdin);

		SetConsoleTitleW(CONSOLE_TITLE);

		// set this to avoid turn japanese texts into question mark
		SetConsoleOutputCP(65001);
		std::locale::global(std::locale(""));

		wprintf(L"%ls Loaded! - By chinosk\n", CONSOLE_TITLE);
		gakumas::vr::WriteVrConsole("Gakumas VRify Loaded! - By KagaminTheMirror");
	}
}

void readProgramConfig() {
	std::vector<std::string> dicts{};
	g_has_config_file = false;
	g_enable_console = false;
	g_useRemoteAssets = false;
	g_useAPIAssets = false;
	g_useAPITextureAssets = false;
	std::ifstream config_stream{ ProgramConfigJson };

	if (!config_stream.is_open())
		return;

	rapidjson::IStreamWrapper wrapper{ config_stream };
	rapidjson::Document document;

	document.ParseStream(wrapper);

	if (!document.HasParseError() && document.IsObject())
	{
		g_has_config_file = true;
		if (document.HasMember("enableConsole") && document["enableConsole"].IsBool()) {
			g_enable_console = document["enableConsole"].GetBool();
		}

		if (document.HasMember("useRemoteAssets") && document["useRemoteAssets"].IsBool()) {
			g_useRemoteAssets = document["useRemoteAssets"].GetBool();
		}

		if (document.HasMember("transRemoteZipUrl") && document["transRemoteZipUrl"].IsString()) {
			g_remoteResourceUrl = document["transRemoteZipUrl"].GetString();
		}

		if (document.HasMember("useAPIAssets") && document["useAPIAssets"].IsBool()) {
			g_useAPIAssets = document["useAPIAssets"].GetBool();
		}

		if (document.HasMember("useAPIAssetsURL") && document["useAPIAssetsURL"].IsString()) {
			g_useAPIAssetsURL = document["useAPIAssetsURL"].GetString();
		}

		if (document.HasMember("useAPITextureAssets") && document["useAPITextureAssets"].IsBool()) {
			g_useAPITextureAssets = document["useAPITextureAssets"].GetBool();
		}

		if (document.HasMember("useAPITextureAssetsURL") && document["useAPITextureAssetsURL"].IsString()) {
			g_useAPITextureAssetsURL = document["useAPITextureAssetsURL"].GetString();
		}

		if (document.HasMember("delTextureRemoteAfterUpdate") && document["delTextureRemoteAfterUpdate"].IsBool()) {
			g_delTextureRemoteAfterUpdate = document["delTextureRemoteAfterUpdate"].GetBool();
		}

	}
	config_stream.close();
}

namespace {
	bool install_vr_hook(
		void*,
		void* target,
		void* detour,
		void** original,
		const char* diagnostic_name) {
		return GakumasVR::Hooks::CreateAndEnable(
			target,
			detour,
			original,
			diagnostic_name);
	}

	DWORD WINAPI plugin_bootstrap_thread(LPVOID) {
		try {
			// The DMM launcher can leave the working directory at System32.
			std::wstring module_name(MAX_PATH, L'\0');
			const auto module_name_size = GetModuleFileNameW(
				nullptr,
				module_name.data(),
				static_cast<DWORD>(module_name.size()));
			if (module_name_size == 0 || module_name_size == module_name.size()) {
				return 0;
			}
			module_name.resize(module_name_size);

			const std::filesystem::path module_path(module_name);
			if (_wcsicmp(module_path.filename().c_str(), L"gakumas.exe") != 0) {
				return 0;
			}

			// Keep the game's process-wide working directory untouched. DMM may
			// choose an unusual CWD, so all plugin paths are rooted explicitly at
			// the executable directory instead.
			gakumasLocalPath = module_path.parent_path() / L"gakumas-local";
			ProgramConfigJson = gakumasLocalPath / L"config.json";
			ConfigJson = gakumasLocalPath / L"localizationConfig.json";
			VrConfigJson = module_path.parent_path() / L"gakumas-vr" / L"config.json";
			readProgramConfig();
			loadConfig(ConfigJson,
				GakumasLocal::Config::ConfigLoadPurpose::StartupMigration);
			GakumasLocal::Config::LoadVrConfig(VrConfigJson, ConfigJson);
			// Freeze both startup decisions after config load. Ordinary VR follows
			// vrEnabled; optional diagnostics additionally require vrDiagnosticsEnabled.
			// The upstream dbgMode remains independent.
			GakumasLocal::Config::vrRuntimeStartupEnabled =
				GakumasLocal::Config::vrEnabled;
			GakumasLocal::Config::vrDiagnosticsStartupEnabled =
				GakumasLocal::Config::vrRuntimeStartupEnabled &&
				GakumasLocal::Config::vrDiagnosticsEnabled;

			if (g_enable_console) {
				create_debug_console();
				start_console();
				printf("Command: %s\n", GetCommandLineA());
			}

			if (GakumasLocal::Config::enabled ||
				GakumasLocal::Config::vrRuntimeStartupEnabled) {
				const bool hook_manager_ready = initHook();
				if (GakumasLocal::Config::vrRuntimeStartupEnabled && hook_manager_ready) {
					gakumas::vr::VrRuntimeConfig runtime_config;
					runtime_config.enabled = true;
					runtime_config.diagnosticsEnabled =
						GakumasLocal::Config::vrDiagnosticsStartupEnabled;
					runtime_config.cameraPoseBridgeEnabled =
						GakumasLocal::Config::vrHeadPoseEnabled &&
						!GakumasLocal::Config::vrNativeOnly;
					runtime_config.stereoProjectionEnabled =
						GakumasLocal::Config::vrStereoEnabled &&
						runtime_config.cameraPoseBridgeEnabled &&
						!GakumasLocal::Config::vrNativeOnly;
					runtime_config.stereoLandscapeOnly =
						GakumasLocal::Config::vrStereoLandscapeOnly;
					runtime_config.stereoRenderScale =
						GakumasLocal::Config::vrStereoRenderScale;
					runtime_config.applicationDirectory = module_path.parent_path();

					gakumas::vr::HookRegistrar registrar;
					registrar.install = install_vr_hook;
					if (!gakumas::vr::StartVrRuntime(std::move(runtime_config), registrar)) {
						OutputDebugStringA("Gakumas VR runtime failed to start.");
					}
				}
			}
		}
		catch (const std::exception& exception) {
			const auto message = std::string("Gakumas VR bootstrap failed: ") + exception.what();
			OutputDebugStringA(message.c_str());
		}
		return 0;
	}
}

// The deterministic host harness has a finite lifetime unlike the game. Give it
// an explicit, non-loader-lock teardown seam so a live OpenXR runtime is never
// abandoned while its host process is returning from wWinMain.
extern "C" __declspec(dllexport) void GakumasVrStopRuntimeForTest() noexcept
{
	gakumas::vr::StopVrRuntime();
}

BOOL WINAPI DllMain(HINSTANCE dllModule, DWORD reason, LPVOID)
{
	if (reason == DLL_PROCESS_ATTACH) {
		DisableThreadLibraryCalls(dllModule);
		const auto bootstrap_thread = CreateThread(
			nullptr,
			0,
			plugin_bootstrap_thread,
			nullptr,
			0,
			nullptr);
		if (bootstrap_thread) {
			CloseHandle(bootstrap_thread);
		}
	}

	// Never perform hook teardown, I/O, or thread coordination under the loader
	// lock. The proxy DLL remains loaded for the lifetime of the game process.
	return TRUE;
}
