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
int hotk = 'u';

void reload_all_data();

std::function<void()> on_hotKey_0;
std::function<void()> g_reload_all_data = reload_all_data;

void readProgramConfig();

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

void checkAndInitConfig(const std::filesystem::path& LocalConfigFile) {
    if (!std::filesystem::exists(ProgramConfigJson)) {
        g_useAPIAssetsURL = GkmsGUII18n::ts("default_assets_check_api");
        GkmsResourceUpdate::saveProgramConfig();
    }
    if (!std::filesystem::exists(LocalConfigFile)) {
        GakumasLocal::Config::SaveConfig(LocalConfigFile.string());
    }
}


void loadConfig(const std::string& configJson,
    GakumasLocal::Config::ConfigLoadPurpose purpose) {
	GakumasLocal::Config::LoadConfig(configJson, purpose);
}

void loadConfig(const std::filesystem::path& filePath,
    GakumasLocal::Config::ConfigLoadPurpose purpose) {
	checkAndInitConfig(filePath);

    std::ifstream file(filePath);
    if (!file.is_open()) {
        GakumasLocal::Log::ErrorFmt("Load config %s failed.\n", filePath.string().c_str());
        loadConfig(std::string("{}"), purpose);
        return;
    }
    std::string fileContent((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    file.close();

    loadConfig(fileContent, purpose);
}

void reload_all_data() {
    readProgramConfig();
	loadConfig(ConfigJson, GakumasLocal::Config::ConfigLoadPurpose::LocalifyReload);
    if (!GakumasLocal::Config::enabled) {
        return;
    }
    GkmsResourceUpdate::GetCurrentResourceVersion(false);
    GkmsResourceUpdate::GetCurrentTextureVersion(false);
	GakumasLocal::Local::LoadData();
	GakumasLocal::MasterLocal::LoadData();
}

bool getCurrentLodingProgress(int* stepTotal, int* stepCurrent, int* currTotal, int* currCurrent) {
    *stepTotal = UnityResolveProgress::assembliesProgress.total;
	*stepCurrent = UnityResolveProgress::assembliesProgress.current;
	*currTotal = UnityResolveProgress::classProgress.total;
	*currCurrent = UnityResolveProgress::classProgress.current;

    return UnityResolveProgress::startInit;
}

void checkDBGKey(int action, int key_code) {
	if (action != WM_KEYDOWN) return;
	static const std::vector<int> targetDbgKeyList = { 38, 38, 40, 40, 37, 39, 37, 39, 66, 65};
    static int currentIndex = 0;
    if (targetDbgKeyList[currentIndex] == key_code) {
        if (currentIndex == targetDbgKeyList.size() - 1) {
            currentIndex = 0;
			GakumasLocal::Config::dbgMode = !GakumasLocal::Config::dbgMode;
			GakumasLocal::Config::SaveConfig(ConfigJson.string());
            GakumasLocal::Log::InfoFmt("Debug Mode: %d", GakumasLocal::Config::dbgMode);
        }
        else {
            currentIndex++;
        }
    }
    else {
        currentIndex = 0;
    }
}

void keyboardEvents(int action, int key_code) {
	// GakumasLocal::Log::DebugFmt("keyboardEvents: %d - %d", action, key_code);
	checkDBGKey(action, key_code);
    GKCamera::on_cam_rawinput_keyboard(action, key_code);
    const auto msg = GakumasLocal::Local::OnKeyDown(action, key_code);
    if (!msg.empty()) {
		GakumasLocal::Log::Info(msg.c_str());
    }
}

namespace GakumasLocal::WinHooks {
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

	void* LoadAssetBundle(const std::string& path) {
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

    void SwitchFullScreen(HWND hWnd) {
        static auto Screen_SetResolution = reinterpret_cast<void (*)(UINT, UINT, UINT, void*)>(
            Il2cppUtils::il2cpp_resolve_icall("UnityEngine.Screen::SetResolution_Injected(System.Int32,System.Int32,UnityEngine.FullScreenMode,UnityEngine.RefreshRate&)"));
        static auto get_Height = reinterpret_cast<int (*)()>(Il2cppUtils::il2cpp_resolve_icall("UnityEngine.Screen::get_height()"));
        static auto get_Width = reinterpret_cast<int (*)()>(Il2cppUtils::il2cpp_resolve_icall("UnityEngine.Screen::get_width()"));

        LONG style = GetWindowLong(hWnd, GWL_STYLE);
        bool currFullScreen = style & WS_POPUP;

		static int savedWidth = -1;
		static int savedHeight = -1;

        int64_t v8[3];
        v8[0] = 0x100000000LL;

        if (currFullScreen) {
            // 取消全屏
            if (savedWidth == -1) {
                savedWidth = 542;
                savedHeight = 990;
            }
            Screen_SetResolution(savedWidth, savedHeight, 2 * !false + 1, v8);
        }
        else {
			savedWidth = get_Width();
			savedHeight = get_Height();
            Screen_SetResolution(GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN), 2 * !true + 1, v8);
        }
    }

    namespace Keyboard {
        std::function<void(int, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD)> mKeyBoardCallBack = nullptr;

        WNDPROC g_pfnOldWndProc = NULL;
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
            runtime.WriteVrLog(std::string("[VR][runtime] GAME_QUIT_FORWARD reason=") +
                gakumas::vr::GameQuitResultName(result.result));
            CallWindowProc(g_pfnOldWndProc, g_quitWindow, WM_CLOSE, g_quitWParam, g_quitLParam);
        }

        void CALLBACK GameQuitTimer(HWND, UINT, UINT_PTR timer, DWORD) {
            if (timer != g_quitTimer || g_quitWindow == nullptr) return;
            if (gakumas::vr::VrRuntime::Instance().PollGameQuit().CanClose()) ForwardGameClose();
        }

        LRESULT CALLBACK WndProcCallback(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
            DWORD SHIFT_key = 0;
            DWORD CTRL_key = 0;
            DWORD ALT_key = 0;
            DWORD SPACE_key = 0;
            DWORD UP_key = 0;
            DWORD DOWN_key = 0;
            DWORD LEFT_key = 0;
            DWORD RIGHT_key = 0;

			// printf("WndProcCallback: 0x%x (%d)\n", uMsg, uMsg);

            switch (uMsg) {
            case WM_INPUT: {
                RAWINPUT rawInput;
                UINT size = sizeof(RAWINPUT);
                if (GetRawInputData((HRAWINPUT)lParam, RID_INPUT, &rawInput, &size, sizeof(RAWINPUTHEADER)) == size) {

                    /* 鼠标事件，后面加上
                    if (rawInput.header.dwType == RIM_TYPEMOUSE)
                    {
                        switch (rawInput.data.mouse.ulButtons) {
                        case 0: {  // move
                            SCCamera::mouseMove(rawInput.data.mouse.lLastX, rawInput.data.mouse.lLastY, 3);
                        }; break;
                        case 4: {  // press
                            SCCamera::mouseMove(0, 0, 1);
                        }; break;
                        case 8: {  // release
                            SCCamera::mouseMove(0, 0, 2);
                        }; break;
                        default: break;
                        }

                        if (rawInput.data.mouse.usButtonFlags == RI_MOUSE_WHEEL) {
                            if (rawInput.data.mouse.usButtonData == 120) {
                                SCCamera::mouseMove(0, 1, 4);
                            }
                            else {
                                SCCamera::mouseMove(0, -1, 4);
                            }
                        }

                    }*/
                }
            }; break;

            case WM_SYSKEYUP:
            case WM_KEYUP: {
                int key = wParam;
                keyboardEvents(uMsg, key);
            }; break;

            case WM_SYSKEYDOWN:
            case WM_KEYDOWN: {
                int key = wParam;

                keyboardEvents(uMsg, key);
                SHIFT_key = GetAsyncKeyState(VK_SHIFT);
                CTRL_key = GetAsyncKeyState(VK_CONTROL);
                ALT_key = GetAsyncKeyState(VK_MENU);
                SPACE_key = GetAsyncKeyState(VK_SPACE);

                UP_key = GetAsyncKeyState(VK_UP);
                DOWN_key = GetAsyncKeyState(VK_DOWN);
                LEFT_key = GetAsyncKeyState(VK_LEFT);
                RIGHT_key = GetAsyncKeyState(VK_RIGHT);

                if (mKeyBoardCallBack != nullptr) {
                    mKeyBoardCallBack(key, SHIFT_key, CTRL_key, ALT_key, SPACE_key, UP_key, DOWN_key, LEFT_key, RIGHT_key);
                }
                if (key == 122) {
					// F11
                    if (GakumasLocal::Config::dmmUnlockSize) {
                        SwitchFullScreen(hWnd);
                    }
                }
                if (key >= 'A' && key <= 'Z')
                {

                    if (GetAsyncKeyState(VK_SHIFT) >= 0) key += 32;

                    if (CTRL_key != 0 && key == hotk)
                    {
                        // fopenExternalPlugin(tlgport);
                        printf("hotKey pressed.\n");
                        if (on_hotKey_0) on_hotKey_0();
                    }

                    SHIFT_key = 0;
                    CTRL_key = 0;
                    ALT_key = 0;
                    SPACE_key = 0;
                    DWORD UP_key = 0;
                    DWORD DOWN_key = 0;
                    DWORD LEFT_key = 0;
                    DWORD RIGHT_key = 0;
                }
            }; break;
            case WM_NCACTIVATE: {
                if (!wParam) {
                    // SCCamera::onKillFocus();
                    return FALSE;
                }
            }; break;
            case WM_KILLFOCUS: {
                // SCCamera::onKillFocus();
                return FALSE;
            }; break;
            case WM_CLOSE: {
                if (g_quitForwarded || g_quitWindow != nullptr) return 0;
                g_quitWindow = hWnd;
                g_quitWParam = wParam;
                g_quitLParam = lParam;
                gakumas::vr::RequestGameQuit(
                    gakumas::vr::GameQuitSource::WindowClose);
                auto& runtime = gakumas::vr::VrRuntime::Instance();
                if (!runtime.PollGameQuit().CanClose()) {
                    g_quitTimer = SetTimer(nullptr, 0, 20, GameQuitTimer);
                    if (g_quitTimer != 0) return 0;
                    runtime.WriteVrLog("[VR][runtime] GAME_QUIT_TIMER failed error=" +
                        std::to_string(GetLastError()));
                    runtime.GameQuitTimerFailed();
                }
                ForwardGameClose();
                return 0;
            }
            case WM_NCDESTROY: {
                if (hWnd == g_quitWindow) {
                    if (g_quitTimer != 0) KillTimer(nullptr, g_quitTimer);
                    g_quitTimer = 0;
                    g_quitWindow = nullptr;
                }
            }; break;
            case WM_NCHITTEST:
            {
                if (GakumasLocal::Config::dmmUnlockSize) {
                    POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                    ScreenToClient(hWnd, &pt);
                    RECT rcClient;
                    GetClientRect(hWnd, &rcClient);
                    const int borderWidth = 8; // 根据需要调整边缘宽度

                    bool left = pt.x < borderWidth;
                    bool right = pt.x >= rcClient.right - borderWidth;
                    bool top = pt.y < borderWidth;
                    bool bottom = pt.y >= rcClient.bottom - borderWidth;

                    if (top && left) return HTTOPLEFT;
                    if (top && right) return HTTOPRIGHT;
                    if (bottom && left) return HTBOTTOMLEFT;
                    if (bottom && right) return HTBOTTOMRIGHT;
                    if (left) return HTLEFT;
                    if (right) return HTRIGHT;
                    if (top) return HTTOP;
                    if (bottom) return HTBOTTOM;
                    return HTCLIENT;
                }
            } break;

            case WM_GETMINMAXINFO:
            {
                if (GakumasLocal::Config::dmmUnlockSize) {
                    LPMINMAXINFO lpMMI = (LPMINMAXINFO)lParam;
                    // 设置最大尺寸为屏幕分辨率，这样就不限制窗口的最大尺寸
                    lpMMI->ptMaxTrackSize.x = GetSystemMetrics(SM_CXSCREEN) * 3;
                    lpMMI->ptMaxTrackSize.y = GetSystemMetrics(SM_CYSCREEN) * 3;
                    // 可选：设置窗口最小尺寸（例如200x200）
                    lpMMI->ptMinTrackSize.x = 200;
                    lpMMI->ptMinTrackSize.y = 200;
                    return 1;
                }
            } break;

            case WM_SYSCOMMAND: {
                if (GakumasLocal::Config::dmmUnlockSize) {
                    if ((wParam & 0xFFF0) == SC_MAXIMIZE) {
                        SwitchFullScreen(hWnd);
                        return 1;
                    }
                }
            } break;

			case WM_NCPAINT: {
                if (GakumasLocal::Config::dmmUnlockSize) {
                    LONG style = GetWindowLong(hWnd, GWL_STYLE);
                    // printf("WM_NCPAINT: 0x%x\n", style);

                    if (!(style & WS_POPUP)) {
                        // 添加可调整大小的边框和最大化按钮
                        style |= WS_THICKFRAME | WS_MAXIMIZEBOX;
                        SetWindowLong(hWnd, GWL_STYLE, style);
                    }
                }

			} break;

            default: break;
            }

            return CallWindowProc(g_pfnOldWndProc, hWnd, uMsg, wParam, lParam);
        }

        void InstallWndProcHook() {
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

            g_pfnOldWndProc = (WNDPROC)GetWindowLongPtr(hWnd, GWLP_WNDPROC);
            SetWindowLongPtr(hWnd, GWLP_WNDPROC, (LONG_PTR)WndProcCallback);
            LONG style = GetWindowLong(hWnd, GWL_STYLE);
            style |= WS_THICKFRAME | WS_MAXIMIZEBOX;
            SetWindowLong(hWnd, GWL_STYLE, style);
            if (GakumasLocal::Config::vrDiagnosticsStartupEnabled) {
                std::ostringstream line;
                line << "WNDPROC_HOOK hwnd=" << hWnd << " miss=0";
                static_cast<void>(gakumas::vr::WriteVrLog(line.str()));
            }
        }

        void UninstallWndProcHook(HWND hWnd)
        {
            SetWindowLongPtr(hWnd, GWLP_WNDPROC, (LONG_PTR)g_pfnOldWndProc);
        }

    }

}

// Existing native harness cannot run Unity bootstrap. Exercise the real window
// hook on its own UnityWndClass window through an explicit test seam.
extern "C" __declspec(dllexport) void GakumasVrInstallQuitHookForTest() noexcept {
    GakumasLocal::WinHooks::Keyboard::InstallWndProcHook();
}
