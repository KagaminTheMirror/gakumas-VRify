#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include "VrLog.hpp"
#include "PerformanceTiming.hpp"
#include "config/VrifyConfig.hpp"

#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <string>
#include <system_error>

namespace gakumas::vr {
namespace {

std::string Timestamp() {
    SYSTEMTIME now{};
    GetLocalTime(&now);

    char value[32]{};
    std::snprintf(
        value,
        sizeof(value),
        "%04u-%02u-%02u %02u:%02u:%02u.%03u",
        static_cast<unsigned>(now.wYear),
        static_cast<unsigned>(now.wMonth),
        static_cast<unsigned>(now.wDay),
        static_cast<unsigned>(now.wHour),
        static_cast<unsigned>(now.wMinute),
        static_cast<unsigned>(now.wSecond),
        static_cast<unsigned>(now.wMilliseconds));
    return value;
}

void DebugFallback(std::string_view message) noexcept {
    char line[1024]{};
    const auto length = static_cast<int>(std::min<std::size_t>(message.size(), sizeof(line) - 16));
    std::snprintf(line, sizeof(line), "[GakumasVR] %.*s\n", length, message.data());
    OutputDebugStringA(line);
}

std::filesystem::path MakeRunLogPath(
    const std::filesystem::path& directory) {
    SYSTEMTIME now{};
    GetLocalTime(&now);

    wchar_t name[64]{};
    std::swprintf(
        name,
        sizeof(name) / sizeof(name[0]),
        L"vr-%04u%02u%02u-%02u%02u%02u.log",
        static_cast<unsigned>(now.wYear),
        static_cast<unsigned>(now.wMonth),
        static_cast<unsigned>(now.wDay),
        static_cast<unsigned>(now.wHour),
        static_cast<unsigned>(now.wMinute),
        static_cast<unsigned>(now.wSecond));
    auto path = directory / name;
    std::error_code existsError;
    if (!std::filesystem::exists(path, existsError)) {
        return path;
    }

    std::swprintf(
        name,
        sizeof(name) / sizeof(name[0]),
        L"vr-%04u%02u%02u-%02u%02u%02u-%lu.log",
        static_cast<unsigned>(now.wYear),
        static_cast<unsigned>(now.wMonth),
        static_cast<unsigned>(now.wDay),
        static_cast<unsigned>(now.wHour),
        static_cast<unsigned>(now.wMinute),
        static_cast<unsigned>(now.wSecond),
        static_cast<unsigned long>(GetCurrentProcessId()));
    return directory / name;
}

} // namespace

void WriteVrConsole(std::string_view message) noexcept {
    if (GetConsoleWindow() == nullptr) {
        return;
    }

    const auto written = std::fwrite(
        message.data(),
        1,
        message.size(),
        stdout);
    if (written != message.size()) {
        DebugFallback(message);
        return;
    }
    if (std::fputc('\n', stdout) == EOF) {
        DebugFallback(message);
        return;
    }
    std::fflush(stdout);
}

VrLog::~VrLog() {
    Close();
}

bool VrLog::Open(
    const std::filesystem::path& applicationDirectory,
    bool fileEnabled) {
    std::lock_guard lock(mutex_);

    if (stream_.is_open()) {
        stream_.flush();
        stream_.close();
    }
    path_.clear();
    fileEnabled_ = fileEnabled;
    opened_ = true;

    if (!fileEnabled_) {
        return true;
    }

    const auto directory = applicationDirectory / L"gakumas-vr" / L"logs";
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) {
        opened_ = false;
        fileEnabled_ = false;
        DebugFallback("[VR][runtime] unable to create log directory");
        return false;
    }

    path_ = MakeRunLogPath(directory);
    stream_.open(path_, std::ios::out | std::ios::trunc);
    if (!stream_.is_open()) {
        opened_ = false;
        fileEnabled_ = false;
        path_.clear();
        DebugFallback("[VR][runtime] unable to open log file");
        return false;
    }

    stream_ << Timestamp() << " [VR][runtime] log opened\n";
    stream_.flush();
    if (!stream_.good()) {
        stream_.close();
        opened_ = false;
        fileEnabled_ = false;
        path_.clear();
        DebugFallback("[VR][runtime] unable to write log file");
        return false;
    }
    return true;
}

void VrLog::Close() noexcept {
    try {
        std::lock_guard lock(mutex_);
        if (stream_.is_open()) {
            stream_.flush();
            stream_.close();
        }
        opened_ = false;
        fileEnabled_ = false;
    } catch (...) {
        DebugFallback("[VR][runtime] log close failed");
    }
}

bool VrLog::Write(std::string_view message) noexcept {
    // Scope is outside the mutex lifetime: summaries cannot re-enter Write
    // under mutex_. The recursive summary write is deliberately not measured.
    static thread_local bool reportingCost = false;
    static thread_local perf::Accumulator writeTiming;
    perf::Scope writeCost(writeTiming,
        GakumasLocal::Config::vrDiagnosticsStartupEnabled && !reportingCost,
        "log.write-total", [this](std::string_view line) noexcept {
            reportingCost = true;
            Write(std::string(line) + " tid=" + std::to_string(GetCurrentThreadId()));
            reportingCost = false;
        });
    try {
        std::lock_guard lock(mutex_);
        if (!opened_) {
            DebugFallback(message);
            return false;
        }
        if (!fileEnabled_) {
            return true;
        }
        if (!stream_.is_open()) {
            DebugFallback(message);
            return false;
        }

        stream_ << Timestamp() << ' ';
        stream_.write(message.data(), static_cast<std::streamsize>(message.size()));
        stream_ << '\n';
        stream_.flush();

        if (!stream_.good()) {
            DebugFallback(message);
            return false;
        }
        return true;
    } catch (...) {
        DebugFallback(message);
        return false;
    }
}

std::filesystem::path VrLog::Path() const {
    std::lock_guard lock(mutex_);
    return path_;
}

} // namespace gakumas::vr
