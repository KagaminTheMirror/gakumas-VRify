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
extern void readProgramConfig();
extern void create_localify_console();
extern std::filesystem::path gakumasLocalPath, ProgramConfigJson, ConfigJson;
extern bool g_enable_console;
std::filesystem::path VrConfigJson = "./gakumas-vr/config.json";
namespace {
	BOOL WINAPI GameQuitConsoleHandler(DWORD controlType)
	{
		switch (controlType) {
		case CTRL_C_EVENT:
		case CTRL_BREAK_EVENT:
			gakumas::vr::RequestGameQuit(gakumas::vr::GameQuitSource::Console);
			return TRUE;
		case CTRL_CLOSE_EVENT:
		case CTRL_LOGOFF_EVENT:
		case CTRL_SHUTDOWN_EVENT:
			gakumas::vr::RequestGameQuit(gakumas::vr::GameQuitSource::Console);
			// Returning (even TRUE) lets Windows terminate this process immediately.
			// Keep this dedicated control thread alive while the Unity window thread
			// runs the existing 2-second XR quit gate and normal game teardown.
			// Do not poll CanClose here: XR completion only admits WM_CLOSE; it does
			// not mean Unity has finished exiting. Windows may impose a shorter cap.
			Sleep(4500);
			return TRUE;
		default:
			break;
		}
		return FALSE;
	}
}
namespace {
    bool install_vr_hook_batch(void*, const gakumas::vr::HookRegistrar::Request* requests, std::size_t count) {
        std::vector<GakumasVR::Hooks::Request> batch;
        batch.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            const auto& r = requests[i];
            batch.push_back({r.target, r.detour, r.original, r.diagnosticName});
        }
        return GakumasVR::Hooks::CreateAndEnableBatch(batch.data(), batch.size());
    }
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
				create_localify_console();
                SetConsoleCtrlHandler(GameQuitConsoleHandler, TRUE);
                gakumas::vr::WriteVrConsole("Gakumas VRify Loaded! - By KagaminTheMirror");
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
					registrar.batch = install_vr_hook_batch;
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
extern "C" __declspec(dllexport) void GakumasVrPumpApplicationFrameForTest() noexcept
{
	gakumas::vr::VrRuntime::Instance().PumpStandaloneFrame();
}

extern "C" __declspec(dllexport) void GakumasVrStopRuntimeForTest() noexcept
{
	gakumas::vr::StopVrRuntime();
}

extern "C" __declspec(dllexport) void GakumasVrRequestQuitForTest(int source) noexcept
{
    if (source == 3) {
        GameQuitConsoleHandler(CTRL_CLOSE_EVENT);
        return;
    }
    gakumas::vr::RequestGameQuit(source == 1 ? gakumas::vr::GameQuitSource::Menu :
        gakumas::vr::GameQuitSource::Localize);
}

extern "C" __declspec(dllexport) int GakumasVrPrepareApplicationFrameForTest() noexcept
{
	auto& runtime = gakumas::vr::VrRuntime::Instance();
	runtime.OnUnityWaitPhase();
	runtime.EnsureGraphicsBegun();
	return runtime.OnUnitySubmitPhase();
}

extern "C" __declspec(dllexport) void GakumasVrRenderApplicationFrameForTest(int eventId) noexcept
{
	gakumas::vr::VrRuntime::Instance().OnGraphicsEndEvent(eventId);
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
