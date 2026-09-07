#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#include "../HookRegistrar.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <vector>

namespace gakumas::vr::d3d11 {

class D3D11Capture final {
public:
    enum class HookInstallResult {
        Installed,
        AlreadyInstalled,
        D3D11NotLoaded,
        InvalidRegistrar,
        Failed,
    };

    struct Snapshot final {
        ID3D11Device* device = nullptr;
        ID3D11DeviceContext* immediateContext = nullptr;
        IDXGISwapChain* swapChain = nullptr;
        HWND outputWindow = nullptr;
        LUID adapterLuid{};
        D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_9_1;
        UINT deviceFlags = 0;
        bool multithreadProtected = false;
        D3D11_TEXTURE2D_DESC frameDescription{};
        std::uint32_t frameDescriptionStableCount = 0;
        std::uint64_t frameGeneration = 0;
        std::uint64_t layoutGeneration = 0;
        std::uint64_t generation = 0;

        Snapshot() = default;
        ~Snapshot();
        Snapshot(const Snapshot&) = delete;
        Snapshot& operator=(const Snapshot&) = delete;
        Snapshot(Snapshot&& other) noexcept;
        Snapshot& operator=(Snapshot&& other) noexcept;

        void Reset() noexcept;
        [[nodiscard]] bool IsComplete() const noexcept;
    };

    struct FrameSnapshot final {
        ID3D11Texture2D* texture = nullptr;
        HWND outputWindow = nullptr;
        D3D11_TEXTURE2D_DESC description{};
        std::uint64_t generation = 0;
        std::uint64_t layoutGeneration = 0;
        bool layoutTransitionPending = false;

        FrameSnapshot() = default;
        ~FrameSnapshot();
        FrameSnapshot(const FrameSnapshot&) = delete;
        FrameSnapshot& operator=(const FrameSnapshot&) = delete;
        FrameSnapshot(FrameSnapshot&& other) noexcept;
        FrameSnapshot& operator=(FrameSnapshot&& other) noexcept;

        void Reset() noexcept;
        [[nodiscard]] bool IsComplete() const noexcept;
    };

    D3D11Capture() = default;
    ~D3D11Capture();

    D3D11Capture(const D3D11Capture&) = delete;
    D3D11Capture& operator=(const D3D11Capture&) = delete;

    // Invoke from a normal worker thread. The registrar must create the hook,
    // populate `original`, then enable it before returning true.
    HookInstallResult InstallHooks(const HookRegistrar& registrar);

    // Waits for a new Unity swap-chain capture. The returned COM references are
    // owned by `snapshot` and released by its destructor.
    bool WaitForSnapshot(
        std::uint64_t& lastGeneration,
        Snapshot& snapshot,
        std::chrono::milliseconds timeout);

    // Returns an AddRef-owned texture containing the most recent Unity frame.
    // The texture itself is filled on Unity's Present thread.
    bool GetLatestFrame(FrameSnapshot& snapshot) const noexcept;

    void WakeWorker() noexcept;
    void Detach() noexcept;

    // While enabled, every Present of the captured Unity swapchain is
    // followed by a clear of its backbuffer to transparent black, so the UI
    // camera repaints onto an alpha-0 base instead of a frozen 3D frame.
    // Driven from the game thread (Grip transparency bridge); read on
    // Unity's Present thread. Arming also schedules a one-shot forensic
    // probe of the presented backbuffer a couple of seconds later.
    void SetTransparentBackbufferClear(bool enabled) noexcept;

    // One-shot forensic sample recorded while the transparent clear is
    // armed: clear statistics plus four raw pixels of the presented
    // backbuffer (corners and centre). Produced on Unity's Present thread,
    // consumed (and reset) by the VR worker for logging.
    struct TransparencyProbe final {
        std::uint64_t clearCount = 0;
        std::uint32_t lastClearError = 0;
        std::uint32_t sampleError = 0;
        std::uint32_t format = 0;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        // Raw little-endian 32-bit texel values, byte order as stored.
        std::array<std::uint32_t, 4> pixels{};
    };
    [[nodiscard]] bool ConsumeTransparencyProbe(
        TransparencyProbe* probe) noexcept;

    // .226: the frozen 3D lives in a URP intermediate color attachment that
    // the UI render pass loads and the final blit re-composites over the
    // backbuffer every frame — clearing the backbuffer alone can never win.
    // The game thread hands over the native ID3D11Texture2D pointers of the
    // big Unity RenderTextures; while the transparent clear is armed, every
    // adopted texture (swapchain-sized, RTV-creatable) is cleared to
    // transparent black after each Present. Passing an empty set releases
    // all adopted references.
    void SetTransparentClearTargets(
        const void* const* textures, std::size_t count) noexcept;

    // Adoption results for logging by the VR worker (same pattern as the
    // transparency probe). Produced once per target-set change.
    struct ClearTargetReport final {
        struct Entry final {
            std::uint64_t texture = 0;
            std::uint32_t width = 0;
            std::uint32_t height = 0;
            std::uint32_t format = 0;
            std::int32_t adopted = 0;
            std::uint32_t hresult = 0;
        };
        std::array<Entry, 16> entries{};
        std::uint32_t count = 0;
        std::uint32_t candidates = 0;
        std::uint32_t adopted = 0;
    };
    [[nodiscard]] bool ConsumeClearTargetReport(
        ClearTargetReport* report) noexcept;

private:
    static constexpr std::uint32_t kRequiredStableFrameCount = 8;

    using CreateDeviceAndSwapChainFn = HRESULT(WINAPI*)(
        IDXGIAdapter*,
        D3D_DRIVER_TYPE,
        HMODULE,
        UINT,
        const D3D_FEATURE_LEVEL*,
        UINT,
        UINT,
        const DXGI_SWAP_CHAIN_DESC*,
        IDXGISwapChain**,
        ID3D11Device**,
        D3D_FEATURE_LEVEL*,
        ID3D11DeviceContext**);

    using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
    using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(
        IDXGISwapChain1*,
        UINT,
        UINT,
        const DXGI_PRESENT_PARAMETERS*);

    static HRESULT WINAPI CreateDeviceAndSwapChainDetour(
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
        ID3D11DeviceContext** immediateContext);

    static HRESULT STDMETHODCALLTYPE PresentDetour(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags);
    static HRESULT STDMETHODCALLTYPE Present1Detour(
        IDXGISwapChain1* swapChain,
        UINT syncInterval,
        UINT presentFlags,
        const DXGI_PRESENT_PARAMETERS* presentParameters);

    bool InstallCreationHooks(HMODULE d3d11Module, const HookRegistrar& registrar);
    bool InstallPresentFallback(HMODULE d3d11Module, const HookRegistrar& registrar);
    bool CreateBootstrapSwapChain(HMODULE d3d11Module, IDXGISwapChain** swapChain);
    void CaptureUnitySwapChain(IDXGISwapChain* swapChain) noexcept;
    void CaptureUnityFrame(IDXGISwapChain* swapChain) noexcept;
    void ClearBackbufferTransparent(IDXGISwapChain* swapChain) noexcept;
    void AdoptTransparentClearTargetsLocked(
        ID3D11Device* device,
        const D3D11_TEXTURE2D_DESC& backbufferDescription) noexcept;
    void ReleaseTransparentClearTargetsLocked() noexcept;
    void MaybeRunTransparencyProbeLocked(
        ID3D11Texture2D* backBuffer,
        const D3D11_TEXTURE2D_DESC& description) noexcept;
    static bool IsUnityTopLevelWindow(HWND window) noexcept;
    static bool IsSameFrameLayout(
        const D3D11_TEXTURE2D_DESC& left,
        const D3D11_TEXTURE2D_DESC& right) noexcept;
    void ResetFrameCandidateLocked() noexcept;
    void CopySnapshotLocked(Snapshot& destination) const noexcept;
    void ReleaseCapturedLocked() noexcept;

    static std::atomic<D3D11Capture*> active_;
    static CreateDeviceAndSwapChainFn originalCreateDeviceAndSwapChain_;
    static PresentFn originalPresent_;
    static Present1Fn originalPresent1_;

    std::mutex installMutex_;
    bool installationAttempted_ = false;
    bool swapChainCreationHookInstalled_ = false;
    bool presentHookInstalled_ = false;
    bool present1HookInstalled_ = false;

    mutable std::mutex captureMutex_;
    std::condition_variable captureChanged_;
    ID3D11Device* capturedDevice_ = nullptr;
    ID3D11DeviceContext* capturedContext_ = nullptr;
    IDXGISwapChain* capturedSwapChain_ = nullptr;
    HWND capturedWindow_ = nullptr;
    LUID capturedAdapterLuid_{};
    D3D_FEATURE_LEVEL capturedFeatureLevel_ = D3D_FEATURE_LEVEL_9_1;
    UINT capturedDeviceFlags_ = 0;
    bool capturedMultithreadProtected_ = false;
    ID3D11Texture2D* capturedFrame_ = nullptr;
    D3D11_TEXTURE2D_DESC capturedFrameDescription_{};
    std::uint32_t frameDescriptionStableCount_ = 0;
    D3D11_TEXTURE2D_DESC frameCandidateDescription_{};
    std::uint32_t frameCandidateStableCount_ = 0;
    bool hasFrameCandidate_ = false;
    std::uint64_t frameGeneration_ = 0;
    std::uint64_t layoutGeneration_ = 0;
    std::uint64_t generation_ = 0;
    std::atomic<bool> transparentBackbufferClear_{false};
    std::atomic<std::uint64_t> transparentClearCount_{0};
    std::atomic<std::uint32_t> transparentClearLastError_{0};
    // Frames until the one-shot probe fires; negative means no probe armed.
    std::atomic<std::int32_t> transparencyProbeCountdown_{-1};
    // Guarded by captureMutex_.
    TransparencyProbe transparencyProbe_{};
    bool transparencyProbeReady_ = false;
    // Intermediate-attachment clear targets; all guarded by captureMutex_.
    struct TransparentClearTarget final {
        ID3D11Texture2D* texture = nullptr; // AddRef-owned
        ID3D11RenderTargetView* view = nullptr;
    };
    std::vector<TransparentClearTarget> transparentClearTargets_;
    std::vector<void*> transparentClearPending_;
    bool transparentClearPendingDirty_ = false;
    ClearTargetReport clearTargetReport_{};
    bool clearTargetReportReady_ = false;
};

} // namespace gakumas::vr::d3d11
