#include "host/VrWindowsPlatform.hpp"
#include "GakumasLocalify/Plugin.h"
#include "GakumasLocalify/Log.h"
#include "GakumasLocalify/Local.h"
#include "GakumasLocalify/MasterLocal.h"
#include "GakumasLocalify/camera/camera.hpp"
#include "vr/config/VrifyConfig.hpp"
#include "vr/VrRuntime.hpp"
#include "GakumasLocalify/Il2cppUtils.hpp"
#include "gkmsGUI/gkmsGUIMain.hpp"
#include "gkmsGUI/GUII18n.hpp"
#include "hooks/HookManager.hpp"
#include "resourceUpdate/resourceUpdate.hpp"

#include <stdinclude.hpp>
#include "deps/UnityResolve/UnityResolve.hpp"
#include <cpprest/details/basic_types.h>
#include <cpprest/details/http_helpers.h>
#include <windowsx.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

extern std::filesystem::path gakumasLocalPath;
extern std::filesystem::path ConfigJson;
extern std::filesystem::path ProgramConfigJson;
extern std::function<void()> on_hotKey_0;
namespace
{
    class WindowsHookInstaller final : public GakumasLocal::HookInstaller
    {
    public:
        explicit WindowsHookInstaller(const std::string& il2cppLibraryPath, const std::string& localizationFilesDir)
        {
            this->m_Il2CppLibrary = GetModuleHandleW(L"GameAssembly.dll");
            this->m_il2cppLibraryPath = il2cppLibraryPath;
            this->localizationFilesDir = localizationFilesDir;
        }

        ~WindowsHookInstaller() override = default;

        void* InstallHook(void* addr, void* hook, void** orig) override
        {
            const auto status = GakumasVR::Hooks::CreateAndEnableHook(addr, hook, orig);
            return reinterpret_cast<void*>(static_cast<std::intptr_t>(status));
        }

        GakumasLocal::OpaqueFunctionPointer LookupSymbol(const char* name) override
        {
            return reinterpret_cast<GakumasLocal::OpaqueFunctionPointer>(GetProcAddress(reinterpret_cast<HMODULE>(m_Il2CppLibrary), name));
        }

    private:
        void* m_Il2CppLibrary;
    };

    using LoadLibraryWFunction = HMODULE(WINAPI*)(LPCWSTR);
    LoadLibraryWFunction g_loadLibraryWOriginal = nullptr;
    std::atomic<HANDLE> g_patchRequestEvent = nullptr;
    std::atomic_bool g_gameAssemblyPatched = false;

    bool IsVuplexWebViewPath(const wchar_t* path) {
        if (!path) {
            return false;
        }

        const auto slash = std::wcsrchr(path, L'\\');
        const auto forwardSlash = std::wcsrchr(path, L'/');
        const auto filename = slash && (!forwardSlash || slash > forwardSlash)
            ? slash + 1
            : (forwardSlash ? forwardSlash + 1 : path);
        return _wcsicmp(filename, L"VuplexWebViewWindows.dll") == 0;
    }

    void PatchGameAssembly() {
        if (!GetModuleHandleW(L"GameAssembly.dll")) {
            GakumasLocal::Log::Error("GameAssembly.dll was not loaded at the Localify milestone.");
            return;
        }

        bool expected = false;
        if (!g_gameAssemblyPatched.compare_exchange_strong(
                expected,
                true,
                std::memory_order_acq_rel)) {
            return;
        }

        auto& plugin = GakumasLocal::Plugin::GetInstance();
        plugin.InstallHook(std::make_unique<WindowsHookInstaller>(
            "GameAssembly.dll",
            gakumasLocalPath.string()));
    }

    DWORD WINAPI PatchWorker(LPVOID parameter) {
        const auto requestEvent = static_cast<HANDLE>(parameter);
        if (WaitForSingleObject(requestEvent, INFINITE) == WAIT_OBJECT_0) {
            try {
                if (GakumasLocal::Config::vrDiagnosticsStartupEnabled) {
                    gakumas::vr::WriteVrLog(
                        "UNITY_BOOTSTRAP_MILESTONE source=vuplex");
                }
                GakumasLocal::Log::Info(
                    "Unity bootstrap milestone: Vuplex loaded.");
                PatchGameAssembly();
            }
            catch (const std::exception& exception) {
                GakumasLocal::Log::ErrorFmt(
                    "Unity hook worker failed: %s",
                    exception.what());
            }
            catch (...) {
                GakumasLocal::Log::Error("Unity hook worker failed with an unknown exception.");
            }
        }
        return 0;
    }

    bool StartPatchWorker() {
        if (g_patchRequestEvent.load(std::memory_order_acquire)) {
            return true;
        }

        const auto requestEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!requestEvent) {
            GakumasLocal::Log::ErrorFmt(
                "CreateEvent for Localify bootstrap failed: %lu",
                GetLastError());
            return false;
        }

        g_patchRequestEvent.store(requestEvent, std::memory_order_release);
        const auto worker = CreateThread(
            nullptr,
            0,
            PatchWorker,
            requestEvent,
            0,
            nullptr);
        if (!worker) {
            g_patchRequestEvent.store(nullptr, std::memory_order_release);
            CloseHandle(requestEvent);
            GakumasLocal::Log::ErrorFmt(
                "CreateThread for Localify bootstrap failed: %lu",
                GetLastError());
            return false;
        }
        CloseHandle(worker);
        return true;
    }

    void RequestGameAssemblyPatch() {
        const auto requestEvent = g_patchRequestEvent.load(std::memory_order_acquire);
        if (requestEvent) {
            SetEvent(requestEvent);
        }
    }

    HMODULE WINAPI LoadLibraryWHook(const wchar_t* path)
    {
        if (!g_loadLibraryWOriginal) {
            return nullptr;
        }

        const auto module = g_loadLibraryWOriginal(path);
        if (module && IsVuplexWebViewPath(path)) {
            // Same milestone as upstream Localify: VuplexWebViewWindows.dll.
            // Do not initialize Unity, allocate, or perform file I/O while the
            // loader is active. The already-running worker does the real work.
            RequestGameAssemblyPatch();
        }
        return module;
    }
}

void unInitHook() {
    GakumasVR::Hooks::Shutdown();
}

bool initHook() {
    if (!GakumasVR::Hooks::Initialize()) {
        return false;
    }

    // The first milestone deliberately proves only the native D3D11/OpenXR
    // path. Do not initialize UnityResolve or install any game-method hooks in
    // native-only mode. Those hooks become relevant only when camera adaptation
    // is explicitly enabled in a later phase.
    if (!GakumasLocal::Config::enabled &&
        (!GakumasLocal::Config::vrRuntimeStartupEnabled ||
         GakumasLocal::Config::vrNativeOnly)) {
        GakumasLocal::Log::Info("Localify/Unity hooks disabled in VR native-only mode.");
        return true;
    }

    if (!StartPatchWorker()) {
        // The VR runtime can still use the process-wide hook owner even if the
        // later Unity/Localify milestone worker could not be created.
        return true;
    }

    const auto kernel32 = GetModuleHandleW(L"kernel32.dll");
    const auto loadLibraryW = kernel32
        ? reinterpret_cast<void*>(GetProcAddress(kernel32, "LoadLibraryW"))
        : nullptr;
    const auto hookStatus = GakumasVR::Hooks::CreateAndEnableHook(
        loadLibraryW,
        reinterpret_cast<void*>(LoadLibraryWHook),
        reinterpret_cast<void**>(&g_loadLibraryWOriginal));
    if (hookStatus != MH_OK) {
        GakumasLocal::Log::ErrorFmt(
            "LoadLibraryW hook failed: %s",
            MH_StatusToString(hookStatus));
        // D3D11/OpenXR capture does not depend on the Localify milestone hook.
        return true;
    }

    if (GakumasLocal::Config::enabled) {
        static std::atomic_bool guiStarting = false;
		on_hotKey_0 = []() {
            bool expected = false;
            if (!guiStarting.compare_exchange_strong(expected, true)) return;
            std::thread([]() {
                printf("GUI START\n");
                guimain();
                guiStarting.store(false, std::memory_order_release);
                printf("GUI END\n");
                }).detach();
			};
    }

    if (GetModuleHandleW(L"VuplexWebViewWindows.dll")) {
        RequestGameAssemblyPatch();
    }

    GakumasLocal::Log::Info("Bootstrap hook initialized.");
    return true;
}


namespace GakumasVR::Localify {
using namespace GakumasLocal;
using Il2cppString = UnityResolve::UnityType::String;
    bool ArgTypeContains(
        const UnityResolve::Method::Arg* arg,
        const char* needle) {
        return arg != nullptr &&
            arg->pType != nullptr &&
            arg->pType->name.find(needle) != std::string::npos;
    }

    UnityResolve::Class* AssetBundleClass() {
        return Il2cppUtils::GetClass(
            "UnityEngine.AssetBundleModule.dll",
            "UnityEngine",
            "AssetBundle");
    }

    void DumpAssetBundleLoadMethodsOnce() {
        static std::atomic_bool dumped{false};
        if (dumped.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        const auto* klass = AssetBundleClass();
        if (!klass) {
            GakumasLocal::Log::Error(
                "ASSETBUNDLE_LOAD_API class=missing");
            return;
        }
        std::ostringstream stream;
        stream << "ASSETBUNDLE_LOAD_API count=" << klass->methods.size();
        for (const auto* method : klass->methods) {
            if (!method ||
                method->name.find("LoadFrom") == std::string::npos) {
                continue;
            }
            stream << " name=" << method->name << "(";
            for (size_t i = 0; i < method->args.size(); ++i) {
                if (i != 0) {
                    stream << ", ";
                }
                if (method->args[i] && method->args[i]->pType) {
                    stream << method->args[i]->pType->name;
                } else {
                    stream << "?";
                }
            }
            stream << ")";
        }
        GakumasLocal::Log::Info(stream.str().c_str());
    }

    UnityResolve::Method* FindLiveAssetBundleLoad(
        const char* name,
        size_t argc,
        const char* firstArgNeedle) {
        const auto* klass = AssetBundleClass();
        if (!klass) {
            return nullptr;
        }
        for (auto* method : klass->methods) {
            if (!method ||
                method->name != name ||
                !method->static_function ||
                method->function == nullptr ||
                method->args.size() != argc) {
                continue;
            }
            if (argc > 0 &&
                !ArgTypeContains(method->args[0], firstArgNeedle)) {
                continue;
            }
            if (argc == 2 &&
                !ArgTypeContains(method->args[1], "UInt32")) {
                continue;
            }
            return method;
        }
        return nullptr;
    }

    void* ReadBundleBytes(const std::filesystem::path& path) {
        static auto* readAllBytes = []() -> void* (*)(Il2cppString*) {
            auto* fileClass = Il2cppUtils::GetClass(
                "mscorlib.dll",
                "System.IO",
                "File");
            if (!fileClass || fileClass->address == nullptr) {
                return nullptr;
            }
            auto* method = Il2cppUtils::il2cpp_class_get_method_from_name(
                fileClass->address,
                "ReadAllBytes",
                1);
            if (!method || method->methodPointer == 0) {
                return nullptr;
            }
            return reinterpret_cast<void* (*)(Il2cppString*)>(
                method->methodPointer);
        }();
        if (readAllBytes != nullptr) {
            return readAllBytes(Il2cppString::New(path.wstring()));
        }

        std::ifstream input(path, std::ios::binary | std::ios::ate);
        if (!input) {
            return nullptr;
        }
        const auto size = static_cast<std::uintptr_t>(input.tellg());
        input.seekg(0, std::ios::beg);
        std::vector<char> bytes(size);
        if (size > 0 &&
            !input.read(bytes.data(), static_cast<std::streamsize>(size))) {
            return nullptr;
        }
        auto* byteClass = Il2cppUtils::GetClass(
            "mscorlib.dll",
            "System",
            "Byte");
        if (!byteClass) {
            return nullptr;
        }
        auto* array = UnityResolve::UnityType::Array<uint8_t>::New(
            byteClass,
            size);
        if (!array) {
            return nullptr;
        }
        if (size > 0) {
            std::memcpy(
                reinterpret_cast<void*>(array->GetData()),
                bytes.data(),
                size);
        }
        return array;
    }

	void* LoadBundle(const std::string& path) {
        const std::filesystem::path abs_path =
            std::filesystem::absolute(path).lexically_normal();
        if (!std::filesystem::is_regular_file(abs_path)) {
            GakumasLocal::Log::ErrorFmt(
                "Asset bundle not found: %s",
                abs_path.string().c_str());
            return nullptr;
        }

        DumpAssetBundleLoadMethodsOnce();

        void* fileBytes = ReadBundleBytes(abs_path);
        if (!fileBytes) {
            GakumasLocal::Log::ErrorFmt(
                "Failed to read asset bundle bytes: %s",
                abs_path.string().c_str());
            return nullptr;
        }

        if (auto* loadFromMemory = FindLiveAssetBundleLoad(
                "LoadFromMemory",
                1,
                "Byte[]")) {
            GakumasLocal::Log::Info(
                "ASSETBUNDLE_LOAD_USE api=LoadFromMemory");
            return loadFromMemory->Invoke<void*>(fileBytes);
        }
        if (auto* loadInternal = FindLiveAssetBundleLoad(
                "LoadFromMemory_Internal",
                2,
                "Byte[]")) {
            GakumasLocal::Log::Info(
                "ASSETBUNDLE_LOAD_USE api=LoadFromMemory_Internal");
            return loadInternal->Invoke<void*>(fileBytes, 0u);
        }

        GakumasLocal::Log::Error(
            "No live sync AssetBundle load API; extra bundle skipped.");
        return nullptr;
	}
        WNDPROC g_pfnOldWndProc = nullptr;
        // Owned exclusively by the Unity window thread. A thread timer obtains
        // a system-assigned ID, so it cannot replace one of Unity's HWND timers.
        HWND g_quitWindow = nullptr;
        UINT_PTR g_quitTimer = 0;
        WPARAM g_quitWParam = 0;
        LPARAM g_quitLParam = 0;
        bool g_quitForwarded = false;

        void ForwardGameClose() {
            if (g_quitForwarded || g_quitWindow == nullptr) return;
            if (g_quitTimer != 0) KillTimer(nullptr, g_quitTimer);
            g_quitTimer = 0;
            g_quitForwarded = true;
            auto& runtime = gakumas::vr::VrRuntime::Instance();
            const auto result = runtime.PollGameQuit();
            runtime.WriteVrLog(std::string("[VR][runtime] GAME_QUIT_CLEANUP_BEGIN reason=") +
                gakumas::vr::GameQuitResultName(result.result));
            gakumas::vr::StopVrRuntime();
            CallWindowProc(g_pfnOldWndProc, g_quitWindow, WM_CLOSE, g_quitWParam, g_quitLParam);
        }

        void CALLBACK GameQuitTimer(HWND, UINT, UINT_PTR timer, DWORD) {
            if (timer != g_quitTimer || g_quitWindow == nullptr) return;
            if (gakumas::vr::VrRuntime::Instance().PollGameQuit().CanClose()) ForwardGameClose();
        }

    bool WindowMessage(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, WNDPROC previous, LRESULT& result) {
        g_pfnOldWndProc = previous;
        result = 0;
        switch (uMsg) {
            case WM_CLOSE: {
                if (g_quitForwarded || g_quitWindow != nullptr) return true;
                g_quitWindow = hWnd;
                g_quitWParam = wParam;
                g_quitLParam = lParam;
                gakumas::vr::RequestGameQuit(
                    gakumas::vr::GameQuitSource::WindowClose);
                auto& runtime = gakumas::vr::VrRuntime::Instance();
                if (!runtime.PollGameQuit().CanClose()) {
                    g_quitTimer = SetTimer(nullptr, 0, 20, GameQuitTimer);
                    if (g_quitTimer != 0) return true;
                    runtime.WriteVrLog("[VR][runtime] GAME_QUIT_TIMER failed error=" +
                        std::to_string(GetLastError()));
                    runtime.GameQuitTimerFailed();
                }
                ForwardGameClose();
                return true;
            }
            case WM_NCDESTROY: {
                if (hWnd == g_quitWindow) {
                    if (g_quitTimer != 0) KillTimer(nullptr, g_quitTimer);
                    g_quitTimer = 0;
                    g_quitWindow = nullptr;
                }
            }; break;
        }
        return false;
    }
        void InstallWindowProcedure(WNDPROC callback, WNDPROC* previous) {
            static std::atomic_bool installed{false};
            if (installed.exchange(true, std::memory_order_acq_rel)) {
                return;
            }

            HWND hWnd = FindWindowW(L"UnityWndClass", L"gakumas");
            if (hWnd != nullptr) {
                DWORD processId = 0;
                GetWindowThreadProcessId(hWnd, &processId);
                if (processId != GetCurrentProcessId()) hWnd = nullptr;
            }
            if (!hWnd) {
                const auto currentProcessId = GetCurrentProcessId();
                HWND candidate = nullptr;
                while ((candidate = FindWindowExW(
                            nullptr,
                            candidate,
                            L"UnityWndClass",
                            nullptr)) != nullptr) {
                    DWORD windowProcessId = 0;
                    GetWindowThreadProcessId(candidate, &windowProcessId);
                    if (windowProcessId == currentProcessId) {
                        hWnd = candidate;
                        break;
                    }
                }
            }
            if (!hWnd) {
                GakumasLocal::Log::Error("WndProc hook skipped: Unity window not found.");
                if (GakumasLocal::Config::vrDiagnosticsStartupEnabled) {
                    static_cast<void>(gakumas::vr::WriteVrLog("WNDPROC_HOOK hwnd=0 miss=1"));
                }
                installed.store(false, std::memory_order_release);
                return;
            }

            *previous = (WNDPROC)GetWindowLongPtr(hWnd, GWLP_WNDPROC);
            SetWindowLongPtr(hWnd, GWLP_WNDPROC, (LONG_PTR)callback);
            LONG style = GetWindowLong(hWnd, GWL_STYLE);
            style |= WS_THICKFRAME | WS_MAXIMIZEBOX;
            SetWindowLong(hWnd, GWL_STYLE, style);
            if (GakumasLocal::Config::vrDiagnosticsStartupEnabled) {
                std::ostringstream line;
                line << "WNDPROC_HOOK hwnd=" << hWnd << " miss=0";
                static_cast<void>(gakumas::vr::WriteVrLog(line.str()));
            }
        }
}
// Existing native harness cannot run Unity bootstrap. Exercise the real window
// hook on its own UnityWndClass window through an explicit test seam.
extern "C" __declspec(dllexport) void GakumasVrInstallQuitHookForTest() noexcept {
    GakumasLocal::WinHooks::Keyboard::InstallWndProcHook();
}
