#include "REX/REX/LOG.h"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace
{
const char* levelToString(const REX::LOG_LEVEL level)
{
    switch (level) {
    case REX::LOG_LEVEL::TRACE:
        return "TRACE";
    case REX::LOG_LEVEL::DEBUG:
        return "DEBUG";
    case REX::LOG_LEVEL::INFO:
        return "INFO";
    case REX::LOG_LEVEL::WARN:
        return "WARN";
    case REX::LOG_LEVEL::ERROR:
        return "ERROR";
    case REX::LOG_LEVEL::CRITICAL:
        return "CRITICAL";
    default:
        return "LOG";
    }
}
}

namespace REX
{
void LOG(const std::source_location a_loc, const LOG_LEVEL a_level, const std::string_view a_fmt)
{
    std::fprintf(
        stderr,
        "[CommonLib:%s] %s:%u %s\n",
        levelToString(a_level),
        a_loc.file_name(),
        static_cast<unsigned>(a_loc.line()),
        std::string(a_fmt).c_str());
}

void LOG(const std::source_location a_loc, const LOG_LEVEL a_level, const std::wstring_view a_fmt)
{
    std::string narrow;
    narrow.reserve(a_fmt.size());
    for (const wchar_t ch : a_fmt) {
        if (ch >= 0 && ch <= 0x7F) {
            narrow.push_back(static_cast<char>(ch));
        } else {
            narrow.push_back('?');
        }
    }
    LOG(a_loc, a_level, narrow);
}
}

namespace REX::IMPL
{
void FAIL(const std::source_location a_loc, const std::string_view a_fmt)
{
    REX::LOG(a_loc, REX::LOG_LEVEL::CRITICAL, a_fmt);
    std::abort();
}

void FAIL(const std::source_location a_loc, const std::wstring_view a_fmt)
{
    REX::LOG(a_loc, REX::LOG_LEVEL::CRITICAL, a_fmt);
    std::abort();
}
}
