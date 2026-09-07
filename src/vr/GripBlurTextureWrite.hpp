#pragma once

namespace gakumas::vr {
struct GripBlurTextureWriteResult {
    bool invoked = false;
    bool readable = false;
    bool matches = false;
    void* previous = nullptr;
    void* readback = nullptr;
};

// Shader uniforms need not be exposed texture properties. Readback is evidence,
// not authorization to cancel a successful setter invocation. The caller owns
// restoration on disarm/removal; this operation issues at most one write.
template<class Read, class Write>
GripBlurTextureWriteResult WriteGripBlurTexture(
    bool hasTextureProperty, void* texture, Read read, Write write) {
    GripBlurTextureWriteResult result;
    if (hasTextureProperty && !read(result.previous)) return result;
    if (!write(texture)) return result;
    result.invoked = true;
    result.readable = hasTextureProperty && read(result.readback);
    result.matches = result.readable && result.readback == texture;
    return result;
}
} // namespace gakumas::vr
