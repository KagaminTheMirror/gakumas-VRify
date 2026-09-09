#include "D3D11Capture.hpp"
#include "../GripTransparencyTrace.hpp"
#include "../PerformanceTiming.hpp"
#include "../frame/FrameLoopDriver.hpp"
#include "../VrRuntime.hpp"
#include "../config/VrifyConfig.hpp"

#include <d3d11_4.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <iterator>
#include <utility>

namespace gakumas::vr::d3d11 {
namespace {

constexpr wchar_t kCaptureWindowClass[] = L"GakumasVrRuntimeDummyWindow";
constexpr wchar_t kUnityWindowClass[] = L"UnityWndClass";

LRESULT CALLBACK BootstrapWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    return DefWindowProcW(window, message, wParam, lParam);
}

template <typename T>
void SafeRelease(T*& value) noexcept {
    if (value != nullptr) {
        value->Release();
        value = nullptr;
    }
}

} // namespace

std::atomic<D3D11Capture*> D3D11Capture::active_{nullptr};
D3D11Capture::CreateDeviceAndSwapChainFn D3D11Capture::originalCreateDeviceAndSwapChain_ = nullptr;
D3D11Capture::PresentFn D3D11Capture::originalPresent_ = nullptr;
D3D11Capture::Present1Fn D3D11Capture::originalPresent1_ = nullptr;

D3D11Capture::Snapshot::~Snapshot() {
    Reset();
}

D3D11Capture::Snapshot::Snapshot(Snapshot&& other) noexcept {
    *this = std::move(other);
}

D3D11Capture::Snapshot& D3D11Capture::Snapshot::operator=(Snapshot&& other) noexcept {
    if (this == &other) {
        return *this;
    }

    Reset();
    device = std::exchange(other.device, nullptr);
    immediateContext = std::exchange(other.immediateContext, nullptr);
    swapChain = std::exchange(other.swapChain, nullptr);
    outputWindow = std::exchange(other.outputWindow, nullptr);
    adapterLuid = other.adapterLuid;
    featureLevel = other.featureLevel;
    deviceFlags = other.deviceFlags;
    multithreadProtected = other.multithreadProtected;
    frameDescription = other.frameDescription;
    frameDescriptionStableCount = other.frameDescriptionStableCount;
    frameGeneration = other.frameGeneration;
    layoutGeneration = other.layoutGeneration;
    generation = other.generation;
    other.adapterLuid = {};
    other.featureLevel = D3D_FEATURE_LEVEL_9_1;
    other.deviceFlags = 0;
    other.multithreadProtected = false;
    other.frameDescription = {};
    other.frameDescriptionStableCount = 0;
    other.frameGeneration = 0;
    other.layoutGeneration = 0;
    other.generation = 0;
    return *this;
}

void D3D11Capture::Snapshot::Reset() noexcept {
    SafeRelease(swapChain);
    SafeRelease(immediateContext);
    SafeRelease(device);
    outputWindow = nullptr;
    adapterLuid = {};
    featureLevel = D3D_FEATURE_LEVEL_9_1;
    deviceFlags = 0;
    multithreadProtected = false;
    frameDescription = {};
    frameDescriptionStableCount = 0;
    frameGeneration = 0;
    layoutGeneration = 0;
    generation = 0;
}

bool D3D11Capture::Snapshot::IsComplete() const noexcept {
    return device != nullptr && immediateContext != nullptr && swapChain != nullptr &&
           outputWindow != nullptr && frameDescription.Width != 0 &&
           frameDescription.Height != 0 && frameGeneration != 0 &&
           layoutGeneration != 0;
}

D3D11Capture::FrameSnapshot::~FrameSnapshot() {
    Reset();
}

D3D11Capture::FrameSnapshot::FrameSnapshot(FrameSnapshot&& other) noexcept {
    *this = std::move(other);
}

D3D11Capture::FrameSnapshot& D3D11Capture::FrameSnapshot::operator=(
    FrameSnapshot&& other) noexcept {
    if (this == &other) {
        return *this;
    }

    Reset();
    texture = std::exchange(other.texture, nullptr);
    outputWindow = std::exchange(other.outputWindow, nullptr);
    description = other.description;
    generation = other.generation;
    layoutGeneration = other.layoutGeneration;
    layoutTransitionPending = other.layoutTransitionPending;
    other.description = {};
    other.generation = 0;
    other.layoutGeneration = 0;
    other.layoutTransitionPending = false;
    return *this;
}

void D3D11Capture::FrameSnapshot::Reset() noexcept {
    SafeRelease(texture);
    outputWindow = nullptr;
    description = {};
    generation = 0;
    layoutGeneration = 0;
    layoutTransitionPending = false;
}

bool D3D11Capture::FrameSnapshot::IsComplete() const noexcept {
    return texture != nullptr && description.Width != 0 &&
           description.Height != 0 && generation != 0 && layoutGeneration != 0;
}

D3D11Capture::~D3D11Capture() {
    Detach();
    std::lock_guard lock(captureMutex_);
    ReleaseCapturedLocked();
}

D3D11Capture::HookInstallResult D3D11Capture::InstallHooks(const HookRegistrar& registrar) {
    std::lock_guard lock(installMutex_);

    if (!registrar.IsValid()) {
        return HookInstallResult::InvalidRegistrar;
    }
    if (installationAttempted_) {
        if (presentHookInstalled_ && present1HookInstalled_) {
            active_.store(this, std::memory_order_release);
            return HookInstallResult::AlreadyInstalled;
        }
        return HookInstallResult::Failed;
    }

    HMODULE d3d11Module = GetModuleHandleW(L"d3d11.dll");
    if (d3d11Module == nullptr) {
        return HookInstallResult::D3D11NotLoaded;
    }

    installationAttempted_ = true;
    active_.store(this, std::memory_order_release);

    const bool creationAvailable = InstallCreationHooks(d3d11Module, registrar);
    const bool presentAvailable = InstallPresentFallback(d3d11Module, registrar);
    // The creation hook is useful only if it beats Unity device creation. A
    // late-loaded proxy must have both Present entry points so it cannot wait
    // forever on a swapchain created before our hook existed.
    if (!presentAvailable) {
        active_.store(nullptr, std::memory_order_release);
        return HookInstallResult::Failed;
    }
    (void)creationAvailable;
    return HookInstallResult::Installed;
}

bool D3D11Capture::InstallCreationHooks(HMODULE d3d11Module, const HookRegistrar& registrar) {
    auto createDeviceAndSwapChain =
        reinterpret_cast<void*>(GetProcAddress(d3d11Module, "D3D11CreateDeviceAndSwapChain"));

    if (createDeviceAndSwapChain != nullptr) {
        swapChainCreationHookInstalled_ = registrar.Install(
            createDeviceAndSwapChain,
            reinterpret_cast<void*>(&CreateDeviceAndSwapChainDetour),
            reinterpret_cast<void**>(&originalCreateDeviceAndSwapChain_),
            "D3D11CreateDeviceAndSwapChain");
        if (!swapChainCreationHookInstalled_) {
            originalCreateDeviceAndSwapChain_ = nullptr;
        }
    }

    return swapChainCreationHookInstalled_;
}

bool D3D11Capture::InstallPresentFallback(HMODULE d3d11Module, const HookRegistrar& registrar) {
    IDXGISwapChain* bootstrapSwapChain = nullptr;
    if (!CreateBootstrapSwapChain(d3d11Module, &bootstrapSwapChain) || bootstrapSwapChain == nullptr) {
        return false;
    }

    void** swapChainVtable = *reinterpret_cast<void***>(bootstrapSwapChain);
    if (swapChainVtable != nullptr) {
        presentHookInstalled_ = registrar.Install(
            swapChainVtable[8],
            reinterpret_cast<void*>(&PresentDetour),
            reinterpret_cast<void**>(&originalPresent_),
            "IDXGISwapChain::Present");
        if (!presentHookInstalled_) {
            originalPresent_ = nullptr;
        }
    }

    IDXGISwapChain1* swapChain1 = nullptr;
    if (SUCCEEDED(bootstrapSwapChain->QueryInterface(IID_PPV_ARGS(&swapChain1))) && swapChain1 != nullptr) {
        void** swapChain1Vtable = *reinterpret_cast<void***>(swapChain1);
        if (swapChain1Vtable != nullptr) {
            present1HookInstalled_ = registrar.Install(
                swapChain1Vtable[22],
                reinterpret_cast<void*>(&Present1Detour),
                reinterpret_cast<void**>(&originalPresent1_),
                "IDXGISwapChain1::Present1");
            if (!present1HookInstalled_) {
                originalPresent1_ = nullptr;
            }
        }
        swapChain1->Release();
    }

    bootstrapSwapChain->Release();
    return presentHookInstalled_ && present1HookInstalled_;
}

bool D3D11Capture::CreateBootstrapSwapChain(HMODULE d3d11Module, IDXGISwapChain** swapChain) {
    if (swapChain == nullptr) {
        return false;
    }
    *swapChain = nullptr;

    auto createDeviceAndSwapChain = originalCreateDeviceAndSwapChain_;
    if (createDeviceAndSwapChain == nullptr) {
        createDeviceAndSwapChain = reinterpret_cast<CreateDeviceAndSwapChainFn>(
            GetProcAddress(d3d11Module, "D3D11CreateDeviceAndSwapChain"));
    }
    if (createDeviceAndSwapChain == nullptr) {
        return false;
    }

    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = BootstrapWindowProc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = kCaptureWindowClass;

    const ATOM atom = RegisterClassExW(&windowClass);
    if (atom == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }

    HWND window = CreateWindowExW(
        0,
        kCaptureWindowClass,
        L"",
        WS_OVERLAPPED,
        0,
        0,
        2,
        2,
        nullptr,
        nullptr,
        instance,
        nullptr);
    if (window == nullptr) {
        if (atom != 0) {
            UnregisterClassW(kCaptureWindowClass, instance);
        }
        return false;
    }

    DXGI_SWAP_CHAIN_DESC description{};
    description.BufferDesc.Width = 2;
    description.BufferDesc.Height = 2;
    description.BufferDesc.RefreshRate.Numerator = 60;
    description.BufferDesc.RefreshRate.Denominator = 1;
    description.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.BufferCount = 1;
    description.OutputWindow = window;
    description.Windowed = TRUE;
    description.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    constexpr std::array featureLevels{
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };

    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    D3D_FEATURE_LEVEL selectedFeatureLevel{};
    HRESULT result = createDeviceAndSwapChain(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        0,
        featureLevels.data(),
        static_cast<UINT>(featureLevels.size()),
        D3D11_SDK_VERSION,
        &description,
        swapChain,
        &device,
        &selectedFeatureLevel,
        &context);

    if (result == E_INVALIDARG) {
        result = createDeviceAndSwapChain(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            0,
            featureLevels.data() + 1,
            static_cast<UINT>(featureLevels.size() - 1),
            D3D11_SDK_VERSION,
            &description,
            swapChain,
            &device,
            &selectedFeatureLevel,
            &context);
    }

    SafeRelease(context);
    SafeRelease(device);
    DestroyWindow(window);
    if (atom != 0) {
        UnregisterClassW(kCaptureWindowClass, instance);
    }
    return SUCCEEDED(result) && *swapChain != nullptr;
}

HRESULT WINAPI D3D11Capture::CreateDeviceAndSwapChainDetour(
    IDXGIAdapter* adapter,
    D3D_DRIVER_TYPE driverType,
    HMODULE software,
    UINT flags,
    const D3D_FEATURE_LEVEL* featureLevels,
    UINT featureLevelCount,
    UINT sdkVersion,
    const DXGI_SWAP_CHAIN_DESC* swapChainDesc,
    IDXGISwapChain** swapChain,
    ID3D11Device** device,
    D3D_FEATURE_LEVEL* featureLevel,
    ID3D11DeviceContext** immediateContext) {
    const auto original = originalCreateDeviceAndSwapChain_;
    if (original == nullptr) {
        return E_FAIL;
    }

    const HRESULT result = original(
        adapter,
        driverType,
        software,
        flags,
        featureLevels,
        featureLevelCount,
        sdkVersion,
        swapChainDesc,
        swapChain,
        device,
        featureLevel,
        immediateContext);

    if (SUCCEEDED(result) && swapChain != nullptr && *swapChain != nullptr) {
        if (auto* active = active_.load(std::memory_order_acquire); active != nullptr) {
            active->CaptureUnitySwapChain(*swapChain);
        }
    }
    return result;
}

HRESULT STDMETHODCALLTYPE D3D11Capture::PresentDetour(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags) {
    static thread_local perf::Accumulator captureTiming, presentTiming;
    const auto sink = [](std::string_view line) noexcept { static_cast<void>(WriteVrLog(line)); };
    const bool timing = GakumasLocal::Config::vrDiagnosticsStartupEnabled;
    perf::Scope captureScope(captureTiming, timing, "present.capture", sink);
    auto* active = active_.load(std::memory_order_acquire);
    if (active != nullptr) {
        active->CaptureUnitySwapChain(swapChain);
        active->CaptureUnityFrame(swapChain);
    }

    const auto original = originalPresent_;
    captureScope.Stop();
    perf::Scope presentScope(presentTiming, timing, "present.original", sink, syncInterval, flags);
    const HRESULT result =
        original != nullptr ? original(swapChain, syncInterval, flags) : E_FAIL;
    presentScope.Stop();
    FrameLoopDriverOnPresent();
    if (active != nullptr &&
        active->transparentBackbufferClear_.load(std::memory_order_acquire)) {
        active->ClearBackbufferTransparent(swapChain);
    }
    return result;
}

HRESULT STDMETHODCALLTYPE D3D11Capture::Present1Detour(
    IDXGISwapChain1* swapChain,
    UINT syncInterval,
    UINT presentFlags,
    const DXGI_PRESENT_PARAMETERS* presentParameters) {
    static thread_local perf::Accumulator captureTiming, presentTiming;
    const auto sink = [](std::string_view line) noexcept { static_cast<void>(WriteVrLog(line)); };
    const bool timing = GakumasLocal::Config::vrDiagnosticsStartupEnabled;
    perf::Scope captureScope(captureTiming, timing, "present1.capture", sink);
    auto* active = active_.load(std::memory_order_acquire);
    if (active != nullptr) {
        active->CaptureUnitySwapChain(swapChain);
        active->CaptureUnityFrame(swapChain);
    }

    const auto original = originalPresent1_;
    captureScope.Stop();
    perf::Scope presentScope(presentTiming, timing, "present1.original", sink, syncInterval, presentFlags);
    const HRESULT result =
        original != nullptr
            ? original(swapChain, syncInterval, presentFlags, presentParameters)
            : E_FAIL;
    presentScope.Stop();
    FrameLoopDriverOnPresent();
    if (active != nullptr &&
        active->transparentBackbufferClear_.load(std::memory_order_acquire)) {
        active->ClearBackbufferTransparent(swapChain);
    }
    return result;
}

void D3D11Capture::CaptureUnitySwapChain(IDXGISwapChain* swapChain) noexcept {
    if (swapChain == nullptr) {
        return;
    }

    {
        std::lock_guard lock(captureMutex_);
        if (capturedSwapChain_ == swapChain) {
            return;
        }
    }

    DXGI_SWAP_CHAIN_DESC swapChainDescription{};
    if (FAILED(swapChain->GetDesc(&swapChainDescription)) ||
        !IsUnityTopLevelWindow(swapChainDescription.OutputWindow)) {
        return;
    }

    ID3D11Device* device = nullptr;
    if (FAILED(swapChain->GetDevice(IID_PPV_ARGS(&device))) || device == nullptr) {
        return;
    }

    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);
    if (context == nullptr) {
        device->Release();
        return;
    }

    // The display worker submits commands through Unity's immediate context.
    // D3D11 serializes that cross-thread use only when this protection is
    // explicitly enabled. This runs on Unity's Present/device-creation thread,
    // before the snapshot becomes visible to the worker.
    bool multithreadProtected = false;
    ID3D11Multithread* multithread = nullptr;
    if (SUCCEEDED(context->QueryInterface(IID_PPV_ARGS(&multithread))) &&
        multithread != nullptr) {
        multithread->SetMultithreadProtected(TRUE);
        multithreadProtected = multithread->GetMultithreadProtected() != FALSE;
        multithread->Release();
    }

    IDXGIDevice* dxgiDevice = nullptr;
    IDXGIAdapter* adapter = nullptr;
    DXGI_ADAPTER_DESC adapterDescription{};
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgiDevice))) || dxgiDevice == nullptr ||
        FAILED(dxgiDevice->GetAdapter(&adapter)) || adapter == nullptr ||
        FAILED(adapter->GetDesc(&adapterDescription))) {
        SafeRelease(adapter);
        SafeRelease(dxgiDevice);
        context->Release();
        device->Release();
        return;
    }

    SafeRelease(adapter);
    SafeRelease(dxgiDevice);
    swapChain->AddRef();

    {
        std::lock_guard lock(captureMutex_);
        ReleaseCapturedLocked();
        capturedDevice_ = device;
        capturedContext_ = context;
        capturedSwapChain_ = swapChain;
        capturedWindow_ = swapChainDescription.OutputWindow;
        capturedAdapterLuid_ = adapterDescription.AdapterLuid;
        capturedFeatureLevel_ = device->GetFeatureLevel();
        capturedDeviceFlags_ = device->GetCreationFlags();
        capturedMultithreadProtected_ = multithreadProtected;
        ++generation_;
    }
    captureChanged_.notify_all();
}

void D3D11Capture::CaptureUnityFrame(IDXGISwapChain* swapChain) noexcept {
    if (swapChain == nullptr) {
        return;
    }

    {
        std::lock_guard lock(captureMutex_);
        if (capturedSwapChain_ != swapChain) {
            return;
        }
    }

    ID3D11Texture2D* backBuffer = nullptr;
    if (FAILED(swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer))) ||
        backBuffer == nullptr) {
        return;
    }

    D3D11_TEXTURE2D_DESC sourceDescription{};
    backBuffer->GetDesc(&sourceDescription);
    if (sourceDescription.Width == 0 || sourceDescription.Height == 0 ||
        sourceDescription.MipLevels != 1 || sourceDescription.ArraySize != 1 ||
        sourceDescription.SampleDesc.Count != 1) {
        backBuffer->Release();
        return;
    }

    bool layoutPublished = false;
    {
        std::lock_guard lock(captureMutex_);
        if (capturedSwapChain_ != swapChain || capturedDevice_ == nullptr ||
            capturedContext_ == nullptr || !capturedMultithreadProtected_) {
            backBuffer->Release();
            return;
        }

        if (capturedFrame_ != nullptr &&
            IsSameFrameLayout(capturedFrameDescription_, sourceDescription)) {
        GripTracePresent(capturedContext_, backBuffer);
            ResetFrameCandidateLocked();
            capturedContext_->CopyResource(capturedFrame_, backBuffer);
            ++frameGeneration_;
        } else {
            D3D11_TEXTURE2D_DESC mirrorDescription = sourceDescription;
            mirrorDescription.Usage = D3D11_USAGE_DEFAULT;
            mirrorDescription.BindFlags = 0;
            mirrorDescription.CPUAccessFlags = 0;
            mirrorDescription.MiscFlags = 0;

            if (!hasFrameCandidate_ ||
                !IsSameFrameLayout(frameCandidateDescription_, mirrorDescription)) {
                frameCandidateDescription_ = mirrorDescription;
                frameCandidateStableCount_ = 1;
                hasFrameCandidate_ = true;
            } else if (frameCandidateStableCount_ < kRequiredStableFrameCount) {
                ++frameCandidateStableCount_;
            }

            if (frameCandidateStableCount_ >= kRequiredStableFrameCount) {
                ID3D11Texture2D* replacement = nullptr;
                if (SUCCEEDED(capturedDevice_->CreateTexture2D(
                        &frameCandidateDescription_, nullptr, &replacement)) &&
                    replacement != nullptr) {
                    // Publish only a complete frame. Until this point readers retain
                    // the last fully copied texture from the previous layout.
                    capturedContext_->CopyResource(replacement, backBuffer);
                    ID3D11Texture2D* previousFrame = capturedFrame_;
                    capturedFrame_ = replacement;
                    capturedFrameDescription_ = frameCandidateDescription_;
                    frameDescriptionStableCount_ = kRequiredStableFrameCount;
                    ResetFrameCandidateLocked();
                    ++frameGeneration_;
                    ++layoutGeneration_;
                    ++generation_;
                    layoutPublished = true;
                    SafeRelease(previousFrame);
                }
            }
        }
        MaybeRunTransparencyProbeLocked(backBuffer, sourceDescription);
    }
    backBuffer->Release();
    if (layoutPublished) {
        captureChanged_.notify_all();
    }
}

void D3D11Capture::ClearBackbufferTransparent(IDXGISwapChain* swapChain) noexcept {
    // Runs on Unity's Present thread, after the original Present, only for
    // the captured Unity swapchain. Clearing to alpha-0 black here means the
    // next frame's UI draws onto a transparent base; CaptureUnityFrame at the
    // top of the next Present copies that alpha through to the Grip quad.
    // Per-frame path: failures skip silently but record the HRESULT for the
    // one-shot transparency probe.
    if (swapChain == nullptr) {
        return;
    }

    {
        std::lock_guard lock(captureMutex_);
        if (capturedSwapChain_ != swapChain) {
            return;
        }
    }

    ID3D11Texture2D* backBuffer = nullptr;
    const HRESULT bufferResult =
        swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (FAILED(bufferResult) || backBuffer == nullptr) {
        transparentClearLastError_.store(
            static_cast<std::uint32_t>(bufferResult),
            std::memory_order_relaxed);
        return;
    }

    ID3D11Device* device = nullptr;
    backBuffer->GetDevice(&device);
    if (device == nullptr) {
        backBuffer->Release();
        return;
    }

    ID3D11RenderTargetView* view = nullptr;
    const HRESULT viewResult =
        device->CreateRenderTargetView(backBuffer, nullptr, &view);
    if (SUCCEEDED(viewResult) && view != nullptr) {
        ID3D11DeviceContext* context = nullptr;
        device->GetImmediateContext(&context);
        if (context != nullptr) {
            constexpr float kTransparentBlack[4]{0.0F, 0.0F, 0.0F, 0.0F};
            context->ClearRenderTargetView(view, kTransparentBlack);
            transparentClearCount_.fetch_add(1, std::memory_order_relaxed);
            // .226: also purge the URP intermediate attachments that hold
            // the frozen 3D — the UI pass loads them and the final blit
            // paints them back over the backbuffer, so the backbuffer
            // clear above can never win on its own.
            {
                std::lock_guard lock(captureMutex_);
                if (transparentClearPendingDirty_) {
                    D3D11_TEXTURE2D_DESC backbufferDescription{};
                    backBuffer->GetDesc(&backbufferDescription);
                    AdoptTransparentClearTargetsLocked(
                        device, backbufferDescription);
                    transparentClearPendingDirty_ = false;
                }
                for (auto& target : transparentClearTargets_) {
                    context->ClearRenderTargetView(
                        target.view, kTransparentBlack);
                }
            }
            context->Release();
        }
        view->Release();
    } else {
        transparentClearLastError_.store(
            static_cast<std::uint32_t>(viewResult), std::memory_order_relaxed);
    }
    device->Release();
    backBuffer->Release();
}

void D3D11Capture::SetTransparentBackbufferClear(bool enabled) noexcept {
    const bool was =
        transparentBackbufferClear_.exchange(enabled, std::memory_order_acq_rel);
    if (enabled && !was) {
        transparentClearCount_.store(0, std::memory_order_relaxed);
        transparentClearLastError_.store(0, std::memory_order_relaxed);
        // ~2s at 60fps: late enough for the game-thread camera clear
        // override to land, so the probe records the steady state.
        transparencyProbeCountdown_.store(120, std::memory_order_release);
    } else if (!enabled && was) {
        transparencyProbeCountdown_.store(-1, std::memory_order_release);
    }
}

bool D3D11Capture::ConsumeTransparencyProbe(TransparencyProbe* probe) noexcept {
    if (probe == nullptr) {
        return false;
    }
    std::lock_guard lock(captureMutex_);
    if (!transparencyProbeReady_) {
        return false;
    }
    *probe = transparencyProbe_;
    transparencyProbeReady_ = false;
    return true;
}

namespace {

// SEH-guarded probe of a game-supplied native pointer: the texture can die
// between the game-thread read and this Present (scene unload window is a
// frame or two). QI validates and AddRefs in one step.
ID3D11Texture2D* TryAcquireTexture(const void* candidate) noexcept {
    if (candidate == nullptr) {
        return nullptr;
    }
    ID3D11Texture2D* texture = nullptr;
    __try {
        auto* unknown =
            static_cast<IUnknown*>(const_cast<void*>(candidate));
        if (FAILED(unknown->QueryInterface(IID_PPV_ARGS(&texture)))) {
            return nullptr;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
    return texture;
}

DXGI_FORMAT RenderTargetViewFormat(DXGI_FORMAT format) noexcept {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    default:
        return format;
    }
}

} // namespace

void D3D11Capture::SetTransparentClearTargets(
    const void* const* textures, std::size_t count) noexcept {
    std::vector<void*> incoming;
    incoming.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        if (textures[index] != nullptr) {
            incoming.push_back(const_cast<void*>(textures[index]));
        }
    }
    std::sort(incoming.begin(), incoming.end());
    incoming.erase(
        std::unique(incoming.begin(), incoming.end()), incoming.end());
    std::lock_guard lock(captureMutex_);
    if (incoming == transparentClearPending_) {
        return;
    }
    transparentClearPending_ = std::move(incoming);
    transparentClearPendingDirty_ = true;
    if (transparentClearPending_.empty()) {
        // Disarm path: drop the adopted references right away instead of
        // waiting for a Present that may never come.
        ReleaseTransparentClearTargetsLocked();
        transparentClearPendingDirty_ = false;
    }
}

void D3D11Capture::ReleaseTransparentClearTargetsLocked() noexcept {
    for (auto& target : transparentClearTargets_) {
        SafeRelease(target.view);
        SafeRelease(target.texture);
    }
    transparentClearTargets_.clear();
}

void D3D11Capture::AdoptTransparentClearTargetsLocked(
    ID3D11Device* device,
    const D3D11_TEXTURE2D_DESC& backbufferDescription) noexcept {
    ReleaseTransparentClearTargetsLocked();
    ClearTargetReport report{};
    report.candidates =
        static_cast<std::uint32_t>(transparentClearPending_.size());
    for (void* candidate : transparentClearPending_) {
        ID3D11Texture2D* texture = TryAcquireTexture(candidate);
        if (texture == nullptr) {
            continue;
        }
        D3D11_TEXTURE2D_DESC description{};
        texture->GetDesc(&description);
        ClearTargetReport::Entry entry{};
        entry.texture = reinterpret_cast<std::uint64_t>(candidate);
        entry.width = description.Width;
        entry.height = description.Height;
        entry.format = static_cast<std::uint32_t>(description.Format);
        bool adopted = false;
        std::uint32_t hresult = 0;
        // .226 taught us the game composits at an internal resolution above
        // the window backbuffer (2160x3840 vs 1081x1921 in portrait) and
        // downsamples in the final blit — exact-size matching adopted
        // nothing. Adopt anything with the backbuffer's aspect ratio at or
        // above its size; eye targets (HMD aspect), squares and mips stay
        // excluded.
        const double backbufferAspect =
            backbufferDescription.Height != 0U
                ? static_cast<double>(backbufferDescription.Width) /
                    static_cast<double>(backbufferDescription.Height)
                : 0.0;
        const double candidateAspect = description.Height != 0U
            ? static_cast<double>(description.Width) /
                static_cast<double>(description.Height)
            : -1.0;
        const bool exactSize =
            description.Width == backbufferDescription.Width &&
            description.Height == backbufferDescription.Height;
        const bool aspectAndSize =
            backbufferAspect > 0.0 &&
            std::abs(candidateAspect - backbufferAspect) <=
                0.02 * backbufferAspect &&
            description.Width >= backbufferDescription.Width &&
            description.Height >= backbufferDescription.Height;
        // .236: ADV composits into a landscape VL target while the window
        // is portrait (and vice versa) and shows it through a UI RawImage.
        // The published set is name-allowlisted to VL surfaces on the game
        // thread, so accepting the swapped aspect cannot hit video RTs.
        const double swappedAspect =
            backbufferAspect > 0.0 ? 1.0 / backbufferAspect : 0.0;
        const bool swappedAspectAndSize =
            swappedAspect > 0.0 &&
            std::abs(candidateAspect - swappedAspect) <=
                0.02 * swappedAspect &&
            description.Width >= backbufferDescription.Height &&
            description.Height >= backbufferDescription.Width;
        if ((exactSize || aspectAndSize || swappedAspectAndSize) &&
            (description.BindFlags & D3D11_BIND_RENDER_TARGET) != 0U) {
            D3D11_RENDER_TARGET_VIEW_DESC viewDescription{};
            viewDescription.Format =
                RenderTargetViewFormat(description.Format);
            viewDescription.ViewDimension =
                description.SampleDesc.Count > 1U
                    ? D3D11_RTV_DIMENSION_TEXTURE2DMS
                    : D3D11_RTV_DIMENSION_TEXTURE2D;
            ID3D11RenderTargetView* view = nullptr;
            const HRESULT viewResult = device->CreateRenderTargetView(
                texture, &viewDescription, &view);
            hresult = static_cast<std::uint32_t>(viewResult);
            if (SUCCEEDED(viewResult) && view != nullptr) {
                transparentClearTargets_.push_back({texture, view});
                adopted = true;
            }
        }
        entry.adopted = adopted ? 1 : 0;
        entry.hresult = hresult;
        if (adopted) {
            ++report.adopted;
        }
        // Report slots favour adopted textures and big rejects so a filter
        // mismatch stays visible in the log.
        if (report.count < report.entries.size() &&
            (adopted || description.Width >= 512U)) {
            report.entries[report.count] = entry;
            ++report.count;
        }
        if (!adopted) {
            texture->Release();
        }
    }
    clearTargetReport_ = report;
    clearTargetReportReady_ = true;
}

bool D3D11Capture::ConsumeClearTargetReport(
    ClearTargetReport* report) noexcept {
    if (report == nullptr) {
        return false;
    }
    std::lock_guard lock(captureMutex_);
    if (!clearTargetReportReady_) {
        return false;
    }
    *report = clearTargetReport_;
    clearTargetReportReady_ = false;
    return true;
}

void D3D11Capture::MaybeRunTransparencyProbeLocked(
    ID3D11Texture2D* backBuffer,
    const D3D11_TEXTURE2D_DESC& description) noexcept {
    // Caller holds captureMutex_ on Unity's Present thread, so
    // capturedDevice_ / capturedContext_ are valid and single-threaded here.
    if (!transparentBackbufferClear_.load(std::memory_order_acquire)) {
        return;
    }
    const std::int32_t countdown =
        transparencyProbeCountdown_.load(std::memory_order_acquire);
    if (countdown < 0) {
        return;
    }
    if (countdown > 0) {
        transparencyProbeCountdown_.store(
            countdown - 1, std::memory_order_release);
        return;
    }
    transparencyProbeCountdown_.store(-1, std::memory_order_release);
    TransparencyProbe probe{};
    probe.clearCount =
        transparentClearCount_.load(std::memory_order_relaxed);
    probe.lastClearError =
        transparentClearLastError_.load(std::memory_order_relaxed);
    probe.format = static_cast<std::uint32_t>(description.Format);
    probe.width = description.Width;
    probe.height = description.Height;
    D3D11_TEXTURE2D_DESC stagingDescription{};
    stagingDescription.Width = 1;
    stagingDescription.Height = 1;
    stagingDescription.MipLevels = 1;
    stagingDescription.ArraySize = 1;
    stagingDescription.Format = description.Format;
    stagingDescription.SampleDesc.Count = 1;
    stagingDescription.Usage = D3D11_USAGE_STAGING;
    stagingDescription.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* staging = nullptr;
    const HRESULT createResult =
        capturedDevice_->CreateTexture2D(&stagingDescription, nullptr, &staging);
    if (FAILED(createResult) || staging == nullptr) {
        probe.sampleError = static_cast<std::uint32_t>(createResult);
    } else {
        const std::uint32_t width = description.Width;
        const std::uint32_t height = description.Height;
        const std::uint32_t xs[4] = {
            8U, width > 9U ? width - 9U : 0U, width / 2U, 8U};
        const std::uint32_t ys[4] = {
            8U, 8U, height / 2U, height > 9U ? height - 9U : 0U};
        for (std::size_t index = 0; index < probe.pixels.size(); ++index) {
            D3D11_BOX box{};
            box.left = xs[index];
            box.right = xs[index] + 1U;
            box.top = ys[index];
            box.bottom = ys[index] + 1U;
            box.front = 0;
            box.back = 1;
            capturedContext_->CopySubresourceRegion(
                staging, 0, 0, 0, 0, backBuffer, 0, &box);
            D3D11_MAPPED_SUBRESOURCE mapped{};
            const HRESULT mapResult =
                capturedContext_->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
            if (SUCCEEDED(mapResult) && mapped.pData != nullptr) {
                std::uint32_t value = 0;
                std::memcpy(&value, mapped.pData, sizeof(value));
                probe.pixels[index] = value;
                capturedContext_->Unmap(staging, 0);
            } else {
                probe.sampleError = static_cast<std::uint32_t>(mapResult);
            }
        }
        staging->Release();
    }
    transparencyProbe_ = probe;
    transparencyProbeReady_ = true;
}

bool D3D11Capture::IsSameFrameLayout(
    const D3D11_TEXTURE2D_DESC& left,
    const D3D11_TEXTURE2D_DESC& right) noexcept {
    return left.Width == right.Width && left.Height == right.Height &&
           left.MipLevels == right.MipLevels && left.ArraySize == right.ArraySize &&
           left.Format == right.Format &&
           left.SampleDesc.Count == right.SampleDesc.Count &&
           left.SampleDesc.Quality == right.SampleDesc.Quality;
}

void D3D11Capture::ResetFrameCandidateLocked() noexcept {
    frameCandidateDescription_ = {};
    frameCandidateStableCount_ = 0;
    hasFrameCandidate_ = false;
}

bool D3D11Capture::IsUnityTopLevelWindow(HWND window) noexcept {
    if (window == nullptr || !IsWindow(window)) {
        return false;
    }

    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    if (processId != GetCurrentProcessId() || GetAncestor(window, GA_ROOT) != window) {
        return false;
    }

    wchar_t className[128]{};
    if (GetClassNameW(window, className, static_cast<int>(std::size(className))) == 0) {
        return false;
    }
    return _wcsicmp(className, kUnityWindowClass) == 0;
}

bool D3D11Capture::WaitForSnapshot(
    std::uint64_t& lastGeneration,
    Snapshot& snapshot,
    std::chrono::milliseconds timeout) {
    std::unique_lock lock(captureMutex_);
    const bool changed = captureChanged_.wait_for(lock, timeout, [&] {
        return generation_ > lastGeneration && capturedDevice_ != nullptr && capturedContext_ != nullptr &&
               capturedSwapChain_ != nullptr && capturedFrame_ != nullptr &&
               frameDescriptionStableCount_ >= kRequiredStableFrameCount &&
               frameGeneration_ != 0;
    });
    if (!changed) {
        return false;
    }

    snapshot.Reset();
    CopySnapshotLocked(snapshot);
    lastGeneration = generation_;
    return snapshot.IsComplete();
}

bool D3D11Capture::GetLatestFrame(FrameSnapshot& snapshot) const noexcept {
    std::lock_guard lock(captureMutex_);
    if (capturedFrame_ == nullptr || frameGeneration_ == 0) {
        return false;
    }

    snapshot.Reset();
    capturedFrame_->AddRef();
    snapshot.texture = capturedFrame_;
    snapshot.outputWindow = capturedWindow_;
    snapshot.description = capturedFrameDescription_;
    snapshot.generation = frameGeneration_;
    snapshot.layoutGeneration = layoutGeneration_;
    snapshot.layoutTransitionPending = hasFrameCandidate_;
    return snapshot.IsComplete();
}

void D3D11Capture::CopySnapshotLocked(Snapshot& destination) const noexcept {
    capturedDevice_->AddRef();
    capturedContext_->AddRef();
    capturedSwapChain_->AddRef();
    destination.device = capturedDevice_;
    destination.immediateContext = capturedContext_;
    destination.swapChain = capturedSwapChain_;
    destination.outputWindow = capturedWindow_;
    destination.adapterLuid = capturedAdapterLuid_;
    destination.featureLevel = capturedFeatureLevel_;
    destination.deviceFlags = capturedDeviceFlags_;
    destination.multithreadProtected = capturedMultithreadProtected_;
    destination.frameDescription = capturedFrameDescription_;
    destination.frameDescriptionStableCount = frameDescriptionStableCount_;
    destination.frameGeneration = frameGeneration_;
    destination.layoutGeneration = layoutGeneration_;
    destination.generation = generation_;
}

void D3D11Capture::ReleaseCapturedLocked() noexcept {
    ReleaseTransparentClearTargetsLocked();
    SafeRelease(capturedFrame_);
    SafeRelease(capturedSwapChain_);
    SafeRelease(capturedContext_);
    SafeRelease(capturedDevice_);
    capturedWindow_ = nullptr;
    capturedAdapterLuid_ = {};
    capturedFeatureLevel_ = D3D_FEATURE_LEVEL_9_1;
    capturedDeviceFlags_ = 0;
    capturedMultithreadProtected_ = false;
    capturedFrameDescription_ = {};
    frameDescriptionStableCount_ = 0;
    ResetFrameCandidateLocked();
    frameGeneration_ = 0;
}

void D3D11Capture::WakeWorker() noexcept {
    captureChanged_.notify_all();
}

void D3D11Capture::Detach() noexcept {
    D3D11Capture* expected = this;
    active_.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel);
    WakeWorker();
}

} // namespace gakumas::vr::d3d11
