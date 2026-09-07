#pragma once

#include <filesystem>
#include <fstream>
#include <mutex>
#include <string_view>

namespace gakumas::vr {

// Sparse user-facing console line. No-ops when AllocConsole has not run.
void WriteVrConsole(std::string_view message) noexcept;

class VrLog final {
public:
    VrLog() = default;
    ~VrLog();

    VrLog(const VrLog&) = delete;
    VrLog& operator=(const VrLog&) = delete;

    // fileEnabled=false: runtime still starts, but no directory or file is
    // created and Write() is a successful no-op. fileEnabled=true: one new
    // gakumas-vr/logs/vr-YYYYMMDD-HHMMSS.log per Open(), no append/rotate/cap.
    [[nodiscard]] bool Open(
        const std::filesystem::path& applicationDirectory,
        bool fileEnabled = true);
    void Close() noexcept;
    // Returns true when the message reached the on-disk log, or when file
    // logging is intentionally off after Open(). false means the caller may
    // retry a lifecycle marker that raced runtime startup.
    bool Write(std::string_view message) noexcept;

    [[nodiscard]] std::filesystem::path Path() const;

private:
    mutable std::mutex mutex_;
    std::filesystem::path path_;
    std::ofstream stream_;
    bool opened_ = false;
    bool fileEnabled_ = false;
};

} // namespace gakumas::vr
