#pragma once
namespace gakumas::vr {
void InstallGripBlurSource() noexcept;
// The existing armed UI Execute intervention supplies the default texture.
// Final material submission also checks the renderer's current armed state.
// Nested/off/eye passes mask, then restore, the outer scope.
class GripBlurSourceScope final {
public:
    GripBlurSourceScope(void* pass, bool allowed) noexcept;
    ~GripBlurSourceScope();
    GripBlurSourceScope(const GripBlurSourceScope&) = delete;
    GripBlurSourceScope& operator=(const GripBlurSourceScope&) = delete;
private:
    void* previous_;
};
} // namespace gakumas::vr
