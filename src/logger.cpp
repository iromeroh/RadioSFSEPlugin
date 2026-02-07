#include "logger.h"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

#include <shlobj.h>

namespace
{
std::filesystem::path fallbackLogPath()
{
    return std::filesystem::path("Data") / "SFSE" / "Plugins" / "RadioSFSE.log";
}
}

bool Logger::initialize()
{
    std::lock_guard<std::mutex> lock(mutex_);

    logPath_ = resolveLogPath();
    std::error_code ec;
    std::filesystem::create_directories(logPath_.parent_path(), ec);

    stream_.open(logPath_, std::ios::out | std::ios::app);
    if (!stream_.is_open()) {
        logPath_ = fallbackLogPath();
        std::filesystem::create_directories(logPath_.parent_path(), ec);
        stream_.open(logPath_, std::ios::out | std::ios::app);
    }

    if (!stream_.is_open()) {
        return false;
    }

    stream_ << '[' << timestamp() << "] [INFO] Logger initialized.\n";
    stream_.flush();
    return true;
}

void Logger::info(const std::string& message)
{
    write("INFO", message);
}

void Logger::warn(const std::string& message)
{
    write("WARN", message);
}

void Logger::error(const std::string& message)
{
    write("ERROR", message);
}

std::filesystem::path Logger::path() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return logPath_;
}

void Logger::write(const char* level, const std::string& message)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!stream_.is_open()) {
        return;
    }

    stream_ << '[' << timestamp() << "] [" << level << "] " << message << '\n';
    stream_.flush();
}

std::filesystem::path Logger::resolveLogPath() const
{
    PWSTR documentsRaw = nullptr;
    const HRESULT hr = SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_DEFAULT, nullptr, &documentsRaw);
    if (FAILED(hr) || documentsRaw == nullptr) {
        if (documentsRaw != nullptr) {
            CoTaskMemFree(documentsRaw);
        }
        return fallbackLogPath();
    }

    const std::filesystem::path base(documentsRaw);
    CoTaskMemFree(documentsRaw);

    return base / "My Games" / "Starfield" / "SFSE" / "Logs" / "RadioSFSE.log";
}

std::string Logger::timestamp()
{
    using Clock = std::chrono::system_clock;
    const auto now = Clock::now();
    const std::time_t tt = Clock::to_time_t(now);

    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &tt);
#else
    localtime_r(&tt, &tm);
#endif

    std::ostringstream out;
    out << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    return out.str();
}
