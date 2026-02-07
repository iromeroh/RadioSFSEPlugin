#pragma once

#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

class Logger
{
public:
    bool initialize();
    void info(const std::string& message);
    void warn(const std::string& message);
    void error(const std::string& message);

    std::filesystem::path path() const;

private:
    void write(const char* level, const std::string& message);
    std::filesystem::path resolveLogPath() const;
    static std::string timestamp();

    mutable std::mutex mutex_;
    std::ofstream stream_;
    std::filesystem::path logPath_;
};
