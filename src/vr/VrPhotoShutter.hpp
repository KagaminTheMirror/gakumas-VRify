#pragma once

#include <cstdint>

namespace gakumas::vr {

// Photography (idol 摄影) scene presenter cache. Published by the
// PhotographyScenePresenter lifecycle hooks on the Unity thread.
void NotePhotographyScenePresenter(void* instance) noexcept;
void ForgetPhotographyScenePresenter(void* instance) noexcept;
// Event-driven fallback: any photography-scene hook whose `self` is not the
// scene presenter (e.g. the scene model's IsInitialized setter) calls this;
// while the cache is empty it runs one rate-limited FindObjectsByType scan
// for the presenter. Never periodic — only fires from photography hooks.
void NotePhotographySceneAnchorEvent() noexcept;

// True while a photo-capable scene is running: an actual Live (presenter with
// a living LivePresenter/player target, produce-end live included) or the
// idol photography scene. Unity thread writes, OpenXR worker reads; this is
// what routes the right-A button between shutter and panel-adjust.
[[nodiscard]] bool PhotoSceneActive() noexcept;

// Unity-thread heartbeat at the Cinemachine boundary: refreshes the scene
// flag from the cached presenters (alive-checked, no scene scans) and fires
// the game's own photo button handler once per queued A press.
void UpdatePhotoSceneAndConsumeShutterRequests() noexcept;

// The game's per-capture thumbnail animation hook calls this: it is the
// ground truth for "a photo was actually taken". Invoke success alone is
// not — OnPhotoButtonAsync silently does nothing at the 50-shot limit.
void NotePhotoCaptured() noexcept;

// Shutter feedback for the worker-side toast. `generation` increments once
// per event; `code` is the PhotoShutterResult of the newest event. Success
// comes only from NotePhotoCaptured; the worker times out a pending press
// that produced neither a capture nor a block. Paused-2D is the Right-A
// shortcut only — Grip's official photo button is not gated here.
constexpr std::uint32_t kPhotoShutterTaken = 1;
constexpr std::uint32_t kPhotoShutterUnavailable = 2;
constexpr std::uint32_t kPhotoShutterPaused2D = 3;
void ReadPhotoShutterResult(
    std::uint32_t& generation, std::uint32_t& code) noexcept;

} // namespace gakumas::vr
