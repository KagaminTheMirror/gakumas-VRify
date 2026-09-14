#pragma once

namespace gakumas::vr {

// Scene handles come from Unity SceneManager, never camera/config generations.
// Unknown lifecycle samples must not turn a transient failure into a retry loop.
struct SceneFailureRecovery {
    int failedScene = 0;
    bool latched = false;
    bool oldSceneExited = false;
    bool cleanupAttempted = false;
    bool cleanupComplete = false;

    bool Fail(int scene) noexcept {
        if (latched) return false;
        failedScene = scene;
        latched = true;
        oldSceneExited = cleanupAttempted = cleanupComplete = false;
        return true;
    }

    void ObserveOldScene(bool known, bool valid) noexcept {
        if (latched && failedScene != 0 && known && !valid)
            oldSceneExited = true;
    }

    bool CanRetry(int scene, bool loaded, bool contentReady,
                  bool sourceReady) const noexcept {
        return latched && oldSceneExited && cleanupComplete &&
            scene != 0 && scene != failedScene && loaded &&
            contentReady && sourceReady;
    }

    void BeginRetry() noexcept { latched = false; }
};

} // namespace gakumas::vr
