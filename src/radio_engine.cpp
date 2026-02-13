#include "radio_engine.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <regex>

#include <windows.h>
#include <dshow.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfplay.h>
#include <mmsystem.h>
#include <wininet.h>
#include <wrl/client.h>

namespace
{
constexpr wchar_t kAlias[] = L"RadioSFSE";
constexpr wchar_t kFxAlias[] = L"RadioSFSE_FX";
constexpr float kMinimumFadeGap = 1.0F;
constexpr float kDefaultVolumePercent = 100.0F;
constexpr float kMaximumVolumePercent = 200.0F;
constexpr float kDefaultVolumeStepPercent = 5.0F;
constexpr std::size_t kMaxResolverBytes = 512 * 1024;
constexpr std::size_t kMaxWrapperTempBytes = 128 * 1024;
constexpr int kMaxResolveDepth = 4;
constexpr DWORD kResolverTimeoutMs = 3500;
constexpr auto kCommandWaitTimeout = std::chrono::milliseconds(5000);
constexpr auto kStreamStartWaitTimeout = std::chrono::milliseconds(10000);
constexpr auto kStreamStartPoll = std::chrono::milliseconds(50);

std::string toLowerCopy(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::string trimAsciiCopy(const std::string& value)
{
    std::size_t start = 0;
    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start])) != 0) {
        ++start;
    }

    std::size_t end = value.size();
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }

    return value.substr(start, end - start);
}

std::string replaceAll(std::string value, const std::string& needle, const std::string& replacement)
{
    if (needle.empty()) {
        return value;
    }

    std::size_t pos = 0;
    while ((pos = value.find(needle, pos)) != std::string::npos) {
        value.replace(pos, needle.size(), replacement);
        pos += replacement.size();
    }
    return value;
}

std::string xmlDecodeEntities(std::string value)
{
    value = replaceAll(std::move(value), "&amp;", "&");
    value = replaceAll(std::move(value), "&lt;", "<");
    value = replaceAll(std::move(value), "&gt;", ">");
    value = replaceAll(std::move(value), "&quot;", "\"");
    value = replaceAll(std::move(value), "&apos;", "'");
    return value;
}

bool isHttpUrl(const std::string& url)
{
    const std::string lower = toLowerCopy(url);
    return lower.starts_with("http://") || lower.starts_with("https://");
}

std::string urlExtensionLower(const std::string& url)
{
    const std::string trimmed = trimAsciiCopy(url);
    const std::size_t endPos = trimmed.find_first_of("?#");
    const std::string path = trimmed.substr(0, endPos);
    const std::size_t slashPos = path.find_last_of('/');
    const std::size_t dotPos = path.find_last_of('.');
    if (dotPos == std::string::npos || (slashPos != std::string::npos && dotPos < slashPos)) {
        return {};
    }

    return toLowerCopy(path.substr(dotPos));
}

bool hasDirectAudioExtension(const std::string& ext)
{
    return ext == ".mp3" || ext == ".aac" || ext == ".m4a" || ext == ".ogg" ||
           ext == ".opus" || ext == ".wav" || ext == ".flac" || ext == ".wma";
}

bool isLikelyWrapperExtension(const std::string& ext)
{
    return ext == ".pls" || ext == ".m3u" || ext == ".m3u8" ||
           ext == ".xspf" || ext == ".rss" || ext == ".atom" || ext == ".xml";
}

bool isLikelyRiskyDirectAudioUrlForMci(const std::string& url)
{
    if (!isHttpUrl(url)) {
        return false;
    }

    const std::string ext = urlExtensionLower(url);
    if (!hasDirectAudioExtension(ext)) {
        return false;
    }

    URL_COMPONENTSA components{};
    components.dwStructSize = sizeof(components);
    components.dwSchemeLength = static_cast<DWORD>(-1);
    components.dwHostNameLength = static_cast<DWORD>(-1);
    components.dwUrlPathLength = static_cast<DWORD>(-1);
    components.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!InternetCrackUrlA(url.c_str(), 0, 0, &components)) {
        return false;
    }

    const bool isHttps = components.nScheme == INTERNET_SCHEME_HTTPS;
    const INTERNET_PORT defaultPort = isHttps ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT;
    return components.nPort != 0 && components.nPort != defaultPort;
}

std::string formatHresult(const HRESULT hr)
{
    char buffer[32]{};
    std::snprintf(buffer, sizeof(buffer), "0x%08X", static_cast<unsigned int>(hr));
    return std::string(buffer);
}

std::string mfStateName(const MFP_MEDIAPLAYER_STATE state)
{
    switch (state) {
    case MFP_MEDIAPLAYER_STATE_EMPTY:
        return "EMPTY";
    case MFP_MEDIAPLAYER_STATE_STOPPED:
        return "STOPPED";
    case MFP_MEDIAPLAYER_STATE_PLAYING:
        return "PLAYING";
    case MFP_MEDIAPLAYER_STATE_PAUSED:
        return "PAUSED";
    case MFP_MEDIAPLAYER_STATE_SHUTDOWN:
        return "SHUTDOWN";
    default:
        return "UNKNOWN";
    }
}

std::string httpsToHttpVariant(const std::string& url)
{
    if (!url.starts_with("https://")) {
        return {};
    }
    return std::string("http://") + url.substr(std::string("https://").size());
}

std::string httpToHttpsVariant(const std::string& url)
{
    if (!url.starts_with("http://")) {
        return {};
    }
    return std::string("https://") + url.substr(std::string("http://").size());
}

std::string buildUrlFromParts(const URL_COMPONENTSA& parts, const std::string& path)
{
    if (parts.lpszHostName == nullptr || parts.dwHostNameLength == 0) {
        return {};
    }

    const bool isHttps = parts.nScheme == INTERNET_SCHEME_HTTPS;
    const std::string scheme = isHttps ? "https://" : "http://";
    const std::string host(parts.lpszHostName, parts.dwHostNameLength);
    if (host.empty()) {
        return {};
    }

    std::string url = scheme + host;
    const INTERNET_PORT defaultPort = isHttps ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT;
    if (parts.nPort != 0 && parts.nPort != defaultPort) {
        url += ":" + std::to_string(parts.nPort);
    }
    if (path.empty()) {
        url += "/";
    } else if (path[0] != '/') {
        url += "/";
        url += path;
    } else {
        url += path;
    }
    return url;
}

std::vector<std::string> makeShoutcastStyleVariants(const std::string& url)
{
    std::vector<std::string> out;
    if (!isHttpUrl(url)) {
        return out;
    }

    URL_COMPONENTSA parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwSchemeLength = static_cast<DWORD>(-1);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!InternetCrackUrlA(url.c_str(), 0, 0, &parts)) {
        return out;
    }

    const std::string path =
        (parts.lpszUrlPath != nullptr && parts.dwUrlPathLength > 0)
            ? std::string(parts.lpszUrlPath, parts.dwUrlPathLength)
            : std::string("/");
    const bool isRootLike = (path == "/" || path == "/;");
    if (isRootLike) {
        return out;
    }

    out.push_back(buildUrlFromParts(parts, "/;"));
    out.push_back(buildUrlFromParts(parts, "/"));
    return out;
}

struct MfEventState
{
    std::atomic<HRESULT> lastError{ S_OK };
    std::atomic<bool> playbackEnded{ false };
};

class MfPlayerCallback final : public IMFPMediaPlayerCallback
{
public:
    explicit MfPlayerCallback(std::shared_ptr<MfEventState> state) :
        state_(std::move(state))
    {
    }

    STDMETHODIMP QueryInterface(REFIID riid, void** object) override
    {
        if (object == nullptr) {
            return E_POINTER;
        }

        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFPMediaPlayerCallback)) {
            *object = static_cast<IMFPMediaPlayerCallback*>(this);
            AddRef();
            return S_OK;
        }

        *object = nullptr;
        return E_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef() override
    {
        return static_cast<ULONG>(++refs_);
    }

    STDMETHODIMP_(ULONG) Release() override
    {
        const long remaining = --refs_;
        if (remaining == 0) {
            delete this;
        }
        return static_cast<ULONG>(remaining);
    }

    void STDMETHODCALLTYPE OnMediaPlayerEvent(MFP_EVENT_HEADER* eventHeader) override
    {
        if (state_ == nullptr || eventHeader == nullptr) {
            return;
        }

        if (FAILED(eventHeader->hrEvent)) {
            state_->lastError.store(eventHeader->hrEvent);
        }
        if (eventHeader->eEventType == MFP_EVENT_TYPE_PLAYBACK_ENDED) {
            state_->playbackEnded.store(true);
        }
    }

private:
    std::atomic<long> refs_{ 1 };
    std::shared_ptr<MfEventState> state_;
};

bool isLikelyBinaryContent(const std::string& text)
{
    if (text.empty()) {
        return false;
    }

    std::size_t controlBytes = 0;
    for (unsigned char c : text) {
        if (c == 0) {
            return true;
        }
        if (c < 32 && c != '\n' && c != '\r' && c != '\t') {
            ++controlBytes;
        }
    }

    return (controlBytes * 100) / text.size() > 3;
}

std::string combineRelativeUrl(const std::string& baseUrl, const std::string& entry)
{
    if (entry.empty()) {
        return {};
    }

    if (isHttpUrl(entry)) {
        return entry;
    }

    if (entry.starts_with("//")) {
        const std::string baseLower = toLowerCopy(baseUrl);
        if (baseLower.starts_with("https://")) {
            return "https:" + entry;
        }
        return "http:" + entry;
    }

    if (!isHttpUrl(baseUrl)) {
        return entry;
    }

    DWORD needed = 0;
    (void)InternetCombineUrlA(baseUrl.c_str(), entry.c_str(), nullptr, &needed, ICU_BROWSER_MODE);
    if (needed == 0 || GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
        return entry;
    }

    std::string combined(static_cast<std::size_t>(needed), '\0');
    if (!InternetCombineUrlA(baseUrl.c_str(), entry.c_str(), combined.data(), &needed, ICU_BROWSER_MODE)) {
        return entry;
    }

    if (needed > 0 && combined[needed - 1] == '\0') {
        --needed;
    }
    combined.resize(static_cast<std::size_t>(needed));
    return combined;
}

struct TextUrlResponse
{
    bool ok{ false };
    std::string body;
    std::string contentType;
    std::string finalUrl;
};

TextUrlResponse fetchUrlText(const std::string& url, const std::function<bool()>& shouldAbort = {})
{
    TextUrlResponse response{};
    if (!isHttpUrl(url)) {
        return response;
    }
    if (shouldAbort && shouldAbort()) {
        return response;
    }

    HINTERNET internet = InternetOpenA("RadioSFSE/1.0", INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    if (internet == nullptr) {
        return response;
    }

    DWORD timeoutMs = kResolverTimeoutMs;
    (void)InternetSetOptionA(internet, INTERNET_OPTION_CONNECT_TIMEOUT, &timeoutMs, sizeof(timeoutMs));
    (void)InternetSetOptionA(internet, INTERNET_OPTION_SEND_TIMEOUT, &timeoutMs, sizeof(timeoutMs));
    (void)InternetSetOptionA(internet, INTERNET_OPTION_RECEIVE_TIMEOUT, &timeoutMs, sizeof(timeoutMs));

    const DWORD flags = INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_NO_COOKIES;
    HINTERNET request = InternetOpenUrlA(internet, url.c_str(), nullptr, 0, flags, 0);
    if (request == nullptr) {
        InternetCloseHandle(internet);
        return response;
    }

    (void)InternetSetOptionA(request, INTERNET_OPTION_CONNECT_TIMEOUT, &timeoutMs, sizeof(timeoutMs));
    (void)InternetSetOptionA(request, INTERNET_OPTION_SEND_TIMEOUT, &timeoutMs, sizeof(timeoutMs));
    (void)InternetSetOptionA(request, INTERNET_OPTION_RECEIVE_TIMEOUT, &timeoutMs, sizeof(timeoutMs));

    DWORD headerSize = 0;
    (void)HttpQueryInfoA(request, HTTP_QUERY_CONTENT_TYPE, nullptr, &headerSize, nullptr);
    if (headerSize > 0 && GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
        std::string contentType(static_cast<std::size_t>(headerSize), '\0');
        if (HttpQueryInfoA(request, HTTP_QUERY_CONTENT_TYPE, contentType.data(), &headerSize, nullptr)) {
            if (headerSize > 0 && contentType[headerSize - 1] == '\0') {
                --headerSize;
            }
            contentType.resize(static_cast<std::size_t>(headerSize));
            response.contentType = trimAsciiCopy(contentType);
        }
    }

    DWORD urlSize = 0;
    (void)InternetQueryOptionA(request, INTERNET_OPTION_URL, nullptr, &urlSize);
    if (urlSize > 0 && GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
        std::string finalUrl(static_cast<std::size_t>(urlSize), '\0');
        if (InternetQueryOptionA(request, INTERNET_OPTION_URL, finalUrl.data(), &urlSize)) {
            if (urlSize > 0 && finalUrl[urlSize - 1] == '\0') {
                --urlSize;
            }
            finalUrl.resize(static_cast<std::size_t>(urlSize));
            response.finalUrl = trimAsciiCopy(finalUrl);
        }
    }

    std::array<char, 4096> buffer{};
    DWORD read = 0;
    while (InternetReadFile(request, buffer.data(), static_cast<DWORD>(buffer.size()), &read) && read > 0) {
        if (shouldAbort && shouldAbort()) {
            break;
        }

        const std::size_t remaining =
            response.body.size() < kMaxResolverBytes ? (kMaxResolverBytes - response.body.size()) : 0;
        if (remaining == 0) {
            break;
        }

        const std::size_t appendSize = std::min<std::size_t>(remaining, static_cast<std::size_t>(read));
        response.body.append(buffer.data(), appendSize);
        if (appendSize < static_cast<std::size_t>(read)) {
            break;
        }
    }

    InternetCloseHandle(request);
    InternetCloseHandle(internet);

    response.ok = !response.body.empty();
    return response;
}

std::string parseM3UFirstUrl(const std::string& body, const std::string& baseUrl)
{
    std::size_t cursor = 0;
    while (cursor <= body.size()) {
        const std::size_t lineEnd = body.find_first_of("\r\n", cursor);
        const std::size_t lineLen = (lineEnd == std::string::npos) ? (body.size() - cursor) : (lineEnd - cursor);
        std::string line = trimAsciiCopy(body.substr(cursor, lineLen));
        cursor = (lineEnd == std::string::npos) ? (body.size() + 1) : (lineEnd + 1);

        if (line.empty() || line[0] == '#') {
            continue;
        }

        line = xmlDecodeEntities(line);
        const std::string resolved = combineRelativeUrl(baseUrl, line);
        if (!resolved.empty()) {
            return resolved;
        }
    }

    return {};
}

std::string parsePLSFirstUrl(const std::string& body, const std::string& baseUrl)
{
    std::map<int, std::string> entries;
    int fallbackIndex = 1000;

    std::size_t cursor = 0;
    while (cursor <= body.size()) {
        const std::size_t lineEnd = body.find_first_of("\r\n", cursor);
        const std::size_t lineLen = (lineEnd == std::string::npos) ? (body.size() - cursor) : (lineEnd - cursor);
        const std::string line = trimAsciiCopy(body.substr(cursor, lineLen));
        cursor = (lineEnd == std::string::npos) ? (body.size() + 1) : (lineEnd + 1);

        if (line.empty() || line[0] == ';' || line[0] == '#') {
            continue;
        }

        const std::size_t eqPos = line.find('=');
        if (eqPos == std::string::npos) {
            continue;
        }

        const std::string key = toLowerCopy(trimAsciiCopy(line.substr(0, eqPos)));
        std::string value = trimAsciiCopy(line.substr(eqPos + 1));
        if (!key.starts_with("file") || value.empty()) {
            continue;
        }

        int index = fallbackIndex++;
        const std::string idxText = trimAsciiCopy(key.substr(4));
        if (!idxText.empty()) {
            try {
                index = std::stoi(idxText);
            } catch (...) {
                index = fallbackIndex++;
            }
        }

        value = xmlDecodeEntities(value);
        entries[index] = combineRelativeUrl(baseUrl, value);
    }

    for (const auto& [index, entry] : entries) {
        (void)index;
        if (!entry.empty()) {
            return entry;
        }
    }

    return {};
}

std::string parseRSSAtomFirstUrl(const std::string& body, const std::string& baseUrl)
{
    static const std::regex enclosurePattern(
        R"(<\s*enclosure\b[^>]*\burl\s*=\s*["']([^"']+)["'])",
        std::regex::icase);
    static const std::regex linkRelHrefPattern(
        R"(<\s*link\b[^>]*\brel\s*=\s*["']enclosure["'][^>]*\bhref\s*=\s*["']([^"']+)["'])",
        std::regex::icase);
    static const std::regex linkHrefRelPattern(
        R"(<\s*link\b[^>]*\bhref\s*=\s*["']([^"']+)["'][^>]*\brel\s*=\s*["']enclosure["'])",
        std::regex::icase);

    std::smatch match;
    if (std::regex_search(body, match, enclosurePattern) && match.size() > 1) {
        return combineRelativeUrl(baseUrl, xmlDecodeEntities(match[1].str()));
    }
    if (std::regex_search(body, match, linkRelHrefPattern) && match.size() > 1) {
        return combineRelativeUrl(baseUrl, xmlDecodeEntities(match[1].str()));
    }
    if (std::regex_search(body, match, linkHrefRelPattern) && match.size() > 1) {
        return combineRelativeUrl(baseUrl, xmlDecodeEntities(match[1].str()));
    }

    return {};
}

std::string parseXSPFFirstUrl(const std::string& body, const std::string& baseUrl)
{
    static const std::regex locationPattern(
        R"(<\s*location\s*>\s*([^<]+?)\s*<\s*/\s*location\s*>)",
        std::regex::icase);

    std::smatch match;
    if (!std::regex_search(body, match, locationPattern) || match.size() <= 1) {
        return {};
    }

    return combineRelativeUrl(baseUrl, xmlDecodeEntities(match[1].str()));
}

std::string resolvePlayableStreamUrl(
    const std::string& inputUrl,
    Logger& logger,
    int depth = 0,
    const std::function<bool()>& shouldAbort = {})
{
    if (shouldAbort && shouldAbort()) {
        return inputUrl;
    }

    if (depth > kMaxResolveDepth) {
        logger.warn("Stream resolver reached max recursion depth for URL: " + inputUrl);
        return inputUrl;
    }

    const std::string trimmed = trimAsciiCopy(inputUrl);
    if (trimmed.empty() || !isHttpUrl(trimmed)) {
        return trimmed;
    }

    const std::string ext = urlExtensionLower(trimmed);
    if (hasDirectAudioExtension(ext)) {
        return trimmed;
    }

    const TextUrlResponse fetched = fetchUrlText(trimmed, shouldAbort);
    if (!fetched.ok) {
        return trimmed;
    }
    if (isLikelyBinaryContent(fetched.body)) {
        return trimmed;
    }

    const std::string finalUrl = fetched.finalUrl.empty() ? trimmed : fetched.finalUrl;
    const std::string lowerContentType = toLowerCopy(fetched.contentType);
    const std::string lowerBody = toLowerCopy(fetched.body);

    const bool looksLikePLS =
        ext == ".pls" || lowerContentType.find("audio/x-scpls") != std::string::npos ||
        lowerBody.find("[playlist]") != std::string::npos;
    const bool looksLikeM3U =
        ext == ".m3u" || ext == ".m3u8" ||
        lowerContentType.find("mpegurl") != std::string::npos ||
        lowerBody.find("#extm3u") != std::string::npos;
    const bool looksLikeXSPF =
        ext == ".xspf" || lowerContentType.find("xspf") != std::string::npos ||
        lowerBody.find("<playlist") != std::string::npos && lowerBody.find("xspf.org/ns/0/") != std::string::npos;
    const bool looksLikeRSSAtom =
        ext == ".rss" || ext == ".atom" || ext == ".xml" ||
        lowerContentType.find("rss") != std::string::npos ||
        lowerContentType.find("atom") != std::string::npos ||
        lowerBody.find("<rss") != std::string::npos ||
        lowerBody.find("<feed") != std::string::npos;

    std::string resolved;
    if (looksLikePLS) {
        resolved = parsePLSFirstUrl(fetched.body, finalUrl);
    } else if (looksLikeM3U) {
        resolved = parseM3UFirstUrl(fetched.body, finalUrl);
    } else if (looksLikeXSPF) {
        resolved = parseXSPFFirstUrl(fetched.body, finalUrl);
    } else if (looksLikeRSSAtom) {
        resolved = parseRSSAtomFirstUrl(fetched.body, finalUrl);
    }

    if (resolved.empty()) {
        return trimmed;
    }

    if (toLowerCopy(resolved) == toLowerCopy(trimmed)) {
        return resolved;
    }

    logger.info("Resolved stream URL: " + trimmed + " -> " + resolved);
    const std::string resolvedExt = urlExtensionLower(resolved);
    if (isLikelyWrapperExtension(resolvedExt)) {
        return resolvePlayableStreamUrl(resolved, logger, depth + 1, shouldAbort);
    }

    return resolved;
}

bool looksLikeWrapperResponse(const TextUrlResponse& fetched, const std::string& requestUrl)
{
    if (!fetched.ok || fetched.body.empty()) {
        return false;
    }

    const std::string ext = urlExtensionLower(requestUrl);
    const std::string lowerContentType = toLowerCopy(fetched.contentType);
    const std::string lowerBody = toLowerCopy(fetched.body);

    const bool looksLikePLS =
        ext == ".pls" || lowerContentType.find("audio/x-scpls") != std::string::npos ||
        lowerBody.find("[playlist]") != std::string::npos;
    const bool looksLikeM3U =
        ext == ".m3u" || ext == ".m3u8" ||
        lowerContentType.find("mpegurl") != std::string::npos ||
        lowerBody.find("#extm3u") != std::string::npos;
    const bool looksLikeXSPF =
        ext == ".xspf" || lowerContentType.find("xspf") != std::string::npos ||
        (lowerBody.find("<playlist") != std::string::npos && lowerBody.find("xspf.org/ns/0/") != std::string::npos);
    const bool looksLikeRSSAtom =
        ext == ".rss" || ext == ".atom" || ext == ".xml" ||
        lowerContentType.find("rss") != std::string::npos ||
        lowerContentType.find("atom") != std::string::npos ||
        lowerBody.find("<rss") != std::string::npos ||
        lowerBody.find("<feed") != std::string::npos;

    return looksLikePLS || looksLikeM3U || looksLikeXSPF || looksLikeRSSAtom;
}

std::string wrapperExtensionForResponse(const TextUrlResponse& fetched, const std::string& requestUrl)
{
    const std::string ext = urlExtensionLower(requestUrl);
    if (isLikelyWrapperExtension(ext)) {
        return ext;
    }

    const std::string lowerContentType = toLowerCopy(fetched.contentType);
    if (lowerContentType.find("audio/x-scpls") != std::string::npos) {
        return ".pls";
    }
    if (lowerContentType.find("mpegurl") != std::string::npos) {
        return ".m3u";
    }
    if (lowerContentType.find("xspf") != std::string::npos) {
        return ".xspf";
    }
    if (lowerContentType.find("rss") != std::string::npos || lowerContentType.find("atom") != std::string::npos) {
        return ".xml";
    }

    return ".m3u";
}

std::optional<std::filesystem::path> writeWrapperResponseToTempFile(
    const TextUrlResponse& fetched,
    const std::string& requestUrl,
    Logger& logger)
{
    if (!fetched.ok || fetched.body.empty()) {
        return std::nullopt;
    }

    const std::size_t bytesToWrite = std::min<std::size_t>(fetched.body.size(), kMaxWrapperTempBytes);
    if (bytesToWrite == 0) {
        return std::nullopt;
    }

    std::filesystem::path tempDir;
    wchar_t tempPathBuf[MAX_PATH + 1]{};
    const DWORD tempLen = GetTempPathW(static_cast<DWORD>(std::size(tempPathBuf)), tempPathBuf);
    if (tempLen > 0 && tempLen < std::size(tempPathBuf)) {
        tempDir = std::filesystem::path(tempPathBuf);
    } else {
        std::error_code ec;
        tempDir = std::filesystem::temp_directory_path(ec);
        if (ec) {
            tempDir = std::filesystem::path(".");
        }
    }

    const std::string fileName = "RadioSFSE_stream_wrapper_" +
        std::to_string(GetCurrentProcessId()) + "_" +
        std::to_string(static_cast<unsigned long long>(GetTickCount64())) +
        wrapperExtensionForResponse(fetched, requestUrl);
    const std::filesystem::path tempFile = tempDir / fileName;

    std::ofstream out(tempFile, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        logger.warn("Stream wrapper fallback failed: could not create temp file " + tempFile.string());
        return std::nullopt;
    }

    out.write(fetched.body.data(), static_cast<std::streamsize>(bytesToWrite));
    out.flush();
    if (!out.good()) {
        out.close();
        std::error_code removeEc;
        std::filesystem::remove(tempFile, removeEc);
        logger.warn("Stream wrapper fallback failed: could not write temp file " + tempFile.string());
        return std::nullopt;
    }

    if (bytesToWrite < fetched.body.size()) {
        logger.warn("Stream wrapper text truncated to " + std::to_string(kMaxWrapperTempBytes) + " bytes.");
    }

    return tempFile;
}

std::string wideToUtf8Local(const std::wstring& text)
{
    if (text.empty()) {
        return {};
    }

    const int needed = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        return {};
    }

    std::string out(static_cast<std::size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), needed, nullptr, nullptr);
    return out;
}

std::string pathToUtf8(const std::filesystem::path& path)
{
#ifdef _WIN32
    return wideToUtf8Local(path.wstring());
#else
    return path.string();
#endif
}

std::filesystem::path defaultRadioRoot()
{
    std::array<wchar_t, 4096> expanded{};
    const DWORD result = ExpandEnvironmentStringsW(
        L"%USERPROFILE%\\OneDrive\\Documentos\\My Games\\Starfield\\Data\\Radio",
        expanded.data(),
        static_cast<DWORD>(expanded.size()));
    if (result > 0 && result < expanded.size()) {
        return std::filesystem::path(expanded.data());
    }

    return std::filesystem::path("Data") / "Radio";
}

std::filesystem::path expandWindowsEnvironmentVariables(const std::string& text)
{
    std::wstring wide;
    if (!text.empty()) {
        const int needed = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
        if (needed > 0) {
            wide.resize(static_cast<std::size_t>(needed));
            MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), wide.data(), needed);
        }
    }

    if (wide.empty()) {
        return {};
    }

    std::array<wchar_t, 4096> expanded{};
    const DWORD result = ExpandEnvironmentStringsW(wide.c_str(), expanded.data(), static_cast<DWORD>(expanded.size()));
    if (result > 0 && result < expanded.size()) {
        return std::filesystem::path(expanded.data());
    }

    return std::filesystem::path(wide);
}
}

struct RadioEngine::MfState
{
    bool comInitialized{ false };
    bool ownsComInitialization{ false };
    bool mfInitialized{ false };
    std::shared_ptr<MfEventState> events{};
    Microsoft::WRL::ComPtr<IMFPMediaPlayerCallback> callback{};
    Microsoft::WRL::ComPtr<IMFPMediaPlayer> player{};
};

struct RadioEngine::DsState
{
    Microsoft::WRL::ComPtr<IGraphBuilder> graph{};
    Microsoft::WRL::ComPtr<IMediaControl> control{};
    Microsoft::WRL::ComPtr<IMediaEvent> events{};
    Microsoft::WRL::ComPtr<IBasicAudio> audio{};
};

RadioEngine::RadioEngine(Logger& logger) :
    logger_(logger)
{
    config_.radioRootPath = defaultRadioRoot();
}

RadioEngine::~RadioEngine()
{
    shutdown();
}

bool RadioEngine::initialize()
{
    std::lock_guard<std::mutex> lock(mutex_);

    logger_.info("[M1] Radio engine initialize start.");
    (void)loadConfig();
    if (!scanLibraryLocked()) {
        logger_.warn("[M2] Initial radio scan failed. Engine will continue and retry on demand.");
    } else {
        logger_.info("[M2] Radio library scan complete. Channels: " + std::to_string(channels_.size()));
    }

    if (!workerRunning_) {
        stopWorker_ = false;
        worker_ = std::thread(&RadioEngine::workerLoop, this);
        workerRunning_ = true;
        logger_.info("[M3] Background worker started.");
    }

    syncCurrentDeviceStateLocked();

    return true;
}

void RadioEngine::shutdown()
{
    bool shouldJoin = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        shouldJoin = workerRunning_;
    }

    if (!shouldJoin) {
        std::lock_guard<std::mutex> lock(mutex_);
        shutdownDirectShowLocked();
        shutdownMediaFoundationLocked();
        return;
    }

    (void)runBoolCommandForDevice(currentDeviceId_, [this]() {
        stopPlaybackDeviceLocked(true);
        stopFxLocked();
        state_ = PlaybackState::Stopped;
        mode_ = PlaybackMode::None;
        trackStartValid_ = false;
        syncCurrentDeviceStateLocked();
        return true;
    });

    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopWorker_ = true;
        cv_.notify_all();
    }

    if (worker_.joinable()) {
        worker_.join();
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        workerRunning_ = false;
        workerThreadId_ = {};
        commandQueue_.clear();
    }

    logger_.info("Radio engine shut down.");
}

bool RadioEngine::changePlaylist(const std::string& channelName, std::uint64_t deviceId)
{
    requestPlayInterrupt(deviceId);
    return runBoolCommandForDevice(deviceId, [this, channelName]() {
        if (config_.autoRescanOnChangePlaylist) {
            scanLibraryLocked();
        }

        const auto channel = lookupChannelLocked(channelName);
        if (!channel.has_value()) {
            logger_.warn("change_playlist failed. Channel not found: " + channelName);
            return false;
        }

        selectedKey_ = channel->key;
        mode_ = PlaybackMode::None;
        state_ = PlaybackState::Stopped;
        songIndex_ = 0;
        transitionIndex_ = 0;
        adIndex_ = 0;
        songsSinceAd_ = 0;
        previousWasSong_ = false;
        currentTrackPath_.clear();
        lastVolume_ = -1;
        lastLeftVolume_ = -1;
        lastRightVolume_ = -1;
        panControlsAvailable_ = true;
        panUnavailableLogged_ = false;
        trackStartValid_ = false;

        stopPlaybackDeviceLocked(true);
        std::string sourceType = "playlist";
        if (channel->isStream) {
            sourceType = "stream";
        } else if (channel->type == ChannelType::Station) {
            sourceType = "station";
        }
        logger_.info("change_playlist selected: " + channel->displayName + " (" + sourceType + ")");
        return true;
    });
}

bool RadioEngine::play(std::uint64_t deviceId)
{
    return runBoolCommandForDevice(deviceId, [this]() {
        if (selectedKey_.empty()) {
            logger_.warn("play failed. No channel selected.");
            return false;
        }

        if (state_ == PlaybackState::Paused) {
            return resumeLocked();
        }

        const auto channelIt = channels_.find(selectedKey_);
        if (channelIt == channels_.end()) {
            logger_.warn("play failed. Selected channel no longer exists.");
            return false;
        }

        const PlaybackMode desiredMode =
            channelIt->second.type == ChannelType::Station ? PlaybackMode::Station : PlaybackMode::Playlist;

        return startCurrentLocked(desiredMode, false);
    });
}

bool RadioEngine::start(std::uint64_t deviceId)
{
    return runBoolCommandForDevice(deviceId, [this]() {
        if (selectedKey_.empty()) {
            logger_.warn("start failed. No channel selected.");
            return false;
        }

        const auto channelIt = channels_.find(selectedKey_);
        if (channelIt == channels_.end()) {
            logger_.warn("start failed. Selected channel no longer exists.");
            return false;
        }

        if (channelIt->second.type != ChannelType::Station) {
            logger_.warn("start requested for a playlist channel. Use play.");
            return false;
        }

        return startCurrentLocked(PlaybackMode::Station, true);
    });
}

bool RadioEngine::pause(std::uint64_t deviceId)
{
    return runBoolCommandForDevice(deviceId, [this]() {
        return pauseLocked();
    });
}

bool RadioEngine::stop(std::uint64_t deviceId)
{
    requestPlayInterrupt(deviceId);
    return runBoolCommandForDevice(deviceId, [this]() {
        stopPlaybackDeviceLocked(true);
        state_ = PlaybackState::Stopped;
        mode_ = PlaybackMode::None;
        songIndex_ = 0;
        transitionIndex_ = 0;
        adIndex_ = 0;
        songsSinceAd_ = 0;
        previousWasSong_ = false;
        currentTrackPath_.clear();
        lastVolume_ = -1;
        lastLeftVolume_ = -1;
        lastRightVolume_ = -1;
        panControlsAvailable_ = true;
        panUnavailableLogged_ = false;
        trackStartValid_ = false;

        logger_.info("stop executed. Playback reset to beginning.");
        return true;
    });
}

bool RadioEngine::forward(std::uint64_t deviceId)
{
    requestPlayInterrupt(deviceId);
    return runBoolCommandForDevice(deviceId, [this]() {
        return forwardLocked();
    });
}

bool RadioEngine::rewind(std::uint64_t deviceId)
{
    requestPlayInterrupt(deviceId);
    return runBoolCommandForDevice(deviceId, [this]() {
        return rewindLocked();
    });
}

bool RadioEngine::rescanLibrary(std::uint64_t deviceId)
{
    return runBoolCommandForDevice(deviceId, [this]() {
        const bool ok = scanLibraryLocked();
        if (ok) {
            logger_.info("Library rescan complete. Channels: " + std::to_string(channels_.size()));
        } else {
            logger_.warn("Library rescan failed.");
        }
        return ok;
    });
}

bool RadioEngine::isPlaying(std::uint64_t deviceId) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (deviceId == currentDeviceId_) {
        return state_ == PlaybackState::Playing;
    }

    const auto it = deviceStates_.find(deviceId);
    if (it == deviceStates_.end()) {
        return false;
    }
    return it->second.state == PlaybackState::Playing;
}

bool RadioEngine::changeToNextSource(int category, std::uint64_t deviceId)
{
    requestPlayInterrupt(deviceId);
    return runBoolCommandForDevice(deviceId, [this, category]() {
        if (config_.autoRescanOnChangePlaylist) {
            scanLibraryLocked();
        }

        struct Candidate
        {
            std::string key;
            std::string displayName;
        };

        std::vector<Candidate> candidates;
        auto addCandidate = [&candidates](const std::string& key, const std::string& displayName) {
            candidates.push_back(Candidate{ key, displayName });
        };

        if (category == 1 || category == 2) {
            for (const auto& [key, entry] : channels_) {
                if (entry.isStream) {
                    continue;
                }
                if (category == 1 && entry.type == ChannelType::Playlist) {
                    addCandidate(key, entry.displayName);
                } else if (category == 2 && entry.type == ChannelType::Station) {
                    addCandidate(key, entry.displayName);
                }
            }
        } else if (category == 3) {
            for (const auto& key : streamOrderKeys_) {
                const auto it = channels_.find(key);
                if (it == channels_.end() || !it->second.isStream) {
                    continue;
                }
                addCandidate(it->first, it->second.displayName);
            }
        } else {
            logger_.warn("changeToNextSource failed. Invalid category: " + std::to_string(category));
            return false;
        }

        if (candidates.empty()) {
            logger_.warn("changeToNextSource failed. No sources for category: " + std::to_string(category));
            return false;
        }

        std::sort(candidates.begin(), candidates.end(), [this](const Candidate& a, const Candidate& b) {
            return toLower(a.displayName) < toLower(b.displayName);
        });

        const auto channelIt = channels_.find(candidates.front().key);
        if (channelIt == channels_.end()) {
            return false;
        }

        selectedKey_ = channelIt->first;
        mode_ = PlaybackMode::None;
        state_ = PlaybackState::Stopped;
        songIndex_ = 0;
        transitionIndex_ = 0;
        adIndex_ = 0;
        songsSinceAd_ = 0;
        previousWasSong_ = false;
        currentTrackPath_.clear();
        lastVolume_ = -1;
        lastLeftVolume_ = -1;
        lastRightVolume_ = -1;
        panControlsAvailable_ = true;
        panUnavailableLogged_ = false;
        trackStartValid_ = false;

        stopPlaybackDeviceLocked(true);

        std::string sourceType = "playlist";
        if (channelIt->second.isStream) {
            sourceType = "stream";
        } else if (channelIt->second.type == ChannelType::Station) {
            sourceType = "station";
        }

        logger_.info("changeToNextSource selected: " + channelIt->second.displayName +
                     " (" + sourceType + ", category=" + std::to_string(category) +
                     "). Playback stopped; waiting for explicit play/start.");
        return true;
    });
}

bool RadioEngine::selectNextSource(int category, std::uint64_t deviceId)
{
    requestPlayInterrupt(deviceId);
    return runBoolCommandForDevice(deviceId, [this, category]() {
        if (config_.autoRescanOnChangePlaylist) {
            scanLibraryLocked();
        }

        struct Candidate
        {
            std::string key;
            std::string displayName;
        };

        std::vector<Candidate> candidates;
        auto addCandidate = [&candidates](const std::string& key, const std::string& displayName) {
            candidates.push_back(Candidate{ key, displayName });
        };

        if (category == 1 || category == 2) {
            for (const auto& [key, entry] : channels_) {
                if (entry.isStream) {
                    continue;
                }
                if (category == 1 && entry.type == ChannelType::Playlist) {
                    addCandidate(key, entry.displayName);
                } else if (category == 2 && entry.type == ChannelType::Station) {
                    addCandidate(key, entry.displayName);
                }
            }
        } else if (category == 3) {
            for (const auto& key : streamOrderKeys_) {
                const auto it = channels_.find(key);
                if (it == channels_.end() || !it->second.isStream) {
                    continue;
                }
                addCandidate(it->first, it->second.displayName);
            }
        } else {
            logger_.warn("selectNextSource failed. Invalid category: " + std::to_string(category));
            return false;
        }

        if (candidates.empty()) {
            logger_.warn("selectNextSource failed. No sources for category: " + std::to_string(category));
            return false;
        }

        std::sort(candidates.begin(), candidates.end(), [this](const Candidate& a, const Candidate& b) {
            return toLower(a.displayName) < toLower(b.displayName);
        });

        std::size_t nextIndex = 0;
        for (std::size_t i = 0; i < candidates.size(); ++i) {
            if (candidates[i].key == selectedKey_) {
                nextIndex = (i + 1) % candidates.size();
                break;
            }
        }

        const auto channelIt = channels_.find(candidates[nextIndex].key);
        if (channelIt == channels_.end()) {
            return false;
        }

        selectedKey_ = channelIt->first;
        mode_ = PlaybackMode::None;
        state_ = PlaybackState::Stopped;
        songIndex_ = 0;
        transitionIndex_ = 0;
        adIndex_ = 0;
        songsSinceAd_ = 0;
        previousWasSong_ = false;
        currentTrackPath_.clear();
        lastVolume_ = -1;
        lastLeftVolume_ = -1;
        lastRightVolume_ = -1;
        panControlsAvailable_ = true;
        panUnavailableLogged_ = false;
        trackStartValid_ = false;

        stopPlaybackDeviceLocked(true);

        std::string sourceType = "playlist";
        if (channelIt->second.isStream) {
            sourceType = "stream";
        } else if (channelIt->second.type == ChannelType::Station) {
            sourceType = "station";
        }

        logger_.info("selectNextSource selected: " + channelIt->second.displayName +
                     " (" + sourceType + ", category=" + std::to_string(category) +
                     "). Playback stopped; waiting for explicit play/start.");
        return true;
    });
}

bool RadioEngine::setPositions(float emitterX, float emitterY, float emitterZ, float playerX, float playerY, float playerZ, std::uint64_t deviceId)
{
    return runBoolCommandForDevice(deviceId, [this, emitterX, emitterY, emitterZ, playerX, playerY, playerZ]() {
        emitterPosition_ = Position{ emitterX, emitterY, emitterZ };
        playerPosition_ = Position{ playerX, playerY, playerZ };
        updateFadeVolumeLocked();
        return true;
    });
}

bool RadioEngine::setFadeParams(float minDistance, float maxDistance, float panDistance, std::uint64_t deviceId)
{
    return runBoolCommandForDevice(deviceId, [this, minDistance, maxDistance, panDistance]() {
        DeviceState& device = ensureDeviceStateLocked(currentDeviceId_);
        if (minDistance < 0.0F || maxDistance < 0.0F || panDistance < 0.0F) {
            device.fadeOverride.enabled = false;
            updateFadeVolumeLocked();
            logger_.info("setFadeParams reset to defaults for deviceId=" + std::to_string(currentDeviceId_));
            return true;
        }

        const float minDist = minDistance;
        const float maxDist = std::max(maxDistance, minDist + kMinimumFadeGap);
        const float panDist = std::max(panDistance, kMinimumFadeGap);

        device.fadeOverride.enabled = true;
        device.fadeOverride.minDistance = minDist;
        device.fadeOverride.maxDistance = maxDist;
        device.fadeOverride.panDistance = panDist;
        updateFadeVolumeLocked();
        logger_.info("setFadeParams deviceId=" + std::to_string(currentDeviceId_) +
                     " min=" + std::to_string(minDist) +
                     " max=" + std::to_string(maxDist) +
                     " pan=" + std::to_string(panDist));
        return true;
    });
}

bool RadioEngine::volumeUp(float step, std::uint64_t deviceId)
{
    return runBoolCommandForDevice(deviceId, [this, step]() {
        DeviceState& device = ensureDeviceStateLocked(currentDeviceId_);
        const float deltaPercent = step > 0.0F ? step : kDefaultVolumeStepPercent;
        const float deltaGain = deltaPercent / kDefaultVolumePercent;
        device.volumeGain = std::clamp(device.volumeGain + deltaGain, 0.0F, 2.0F);
        updateFadeVolumeLocked();
        logger_.info("volumeUp deviceId=" + std::to_string(currentDeviceId_) +
                     " gain=" + std::to_string(device.volumeGain) +
                     " volume=" + std::to_string(device.volumeGain * kDefaultVolumePercent));
        return true;
    });
}

bool RadioEngine::volumeDown(float step, std::uint64_t deviceId)
{
    return runBoolCommandForDevice(deviceId, [this, step]() {
        DeviceState& device = ensureDeviceStateLocked(currentDeviceId_);
        const float deltaPercent = step > 0.0F ? step : kDefaultVolumeStepPercent;
        const float deltaGain = deltaPercent / kDefaultVolumePercent;
        device.volumeGain = std::clamp(device.volumeGain - deltaGain, 0.0F, 2.0F);
        updateFadeVolumeLocked();
        logger_.info("volumeDown deviceId=" + std::to_string(currentDeviceId_) +
                     " gain=" + std::to_string(device.volumeGain) +
                     " volume=" + std::to_string(device.volumeGain * kDefaultVolumePercent));
        return true;
    });
}

float RadioEngine::getVolume(std::uint64_t deviceId) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = deviceStates_.find(deviceId == currentDeviceId_ ? currentDeviceId_ : deviceId);
    if (it == deviceStates_.end()) {
        return kDefaultVolumePercent;
    }

    return std::clamp(it->second.volumeGain * kDefaultVolumePercent, 0.0F, kMaximumVolumePercent);
}

bool RadioEngine::setVolume(float volume, std::uint64_t deviceId)
{
    return runBoolCommandForDevice(deviceId, [this, volume]() {
        DeviceState& device = ensureDeviceStateLocked(currentDeviceId_);
        const float clamped = std::clamp(volume, 0.0F, kMaximumVolumePercent);
        device.volumeGain = clamped / kDefaultVolumePercent;
        updateFadeVolumeLocked();
        logger_.info("setVolume deviceId=" + std::to_string(currentDeviceId_) +
                     " volume=" + std::to_string(clamped));
        return true;
    });
}

std::string RadioEngine::getTrack(std::uint64_t deviceId) const
{
    std::lock_guard<std::mutex> lock(mutex_);

    std::string key;
    std::size_t songIndex = 0;
    std::filesystem::path trackPath;
    if (deviceId == currentDeviceId_) {
        key = selectedKey_;
        songIndex = songIndex_;
        trackPath = currentTrackPath_;
    } else {
        const auto stateIt = deviceStates_.find(deviceId);
        if (stateIt == deviceStates_.end()) {
            return {};
        }

        key = stateIt->second.selectedKey;
        songIndex = stateIt->second.songIndex;
        trackPath = stateIt->second.currentTrackPath;
    }

    if (key.empty()) {
        return {};
    }

    const auto channelIt = channels_.find(key);
    if (channelIt == channels_.end()) {
        return {};
    }

    if (channelIt->second.isStream) {
        return "na";
    }

    if (!trackPath.empty()) {
        return pathToUtf8(trackPath.filename());
    }

    if (songIndex < channelIt->second.songs.size()) {
        return pathToUtf8(channelIt->second.songs[songIndex].filename());
    }

    return {};
}

bool RadioEngine::setTrack(const std::string& trackBasename, std::uint64_t deviceId)
{
    requestPlayInterrupt(deviceId);
    return runBoolCommandForDevice(deviceId, [this, trackBasename]() {
        if (selectedKey_.empty()) {
            logger_.warn("setTrack failed. No source selected.");
            return false;
        }

        const auto channelIt = channels_.find(selectedKey_);
        if (channelIt == channels_.end()) {
            logger_.warn("setTrack failed. Selected source no longer exists.");
            return false;
        }

        const auto& channel = channelIt->second;
        if (channel.isStream) {
            logger_.warn("setTrack failed. Streaming source has no local track list.");
            return false;
        }

        const std::string needle = toLower(trim(trackBasename));
        if (needle.empty()) {
            logger_.warn("setTrack failed. Empty track basename.");
            return false;
        }

        std::size_t foundIndex = channel.songs.size();
        for (std::size_t i = 0; i < channel.songs.size(); ++i) {
            const std::string fileNameLower = toLower(pathToUtf8(channel.songs[i].filename()));
            const std::string stemLower = toLower(pathToUtf8(channel.songs[i].stem()));
            if (needle == fileNameLower || needle == stemLower) {
                foundIndex = i;
                break;
            }
        }

        if (foundIndex >= channel.songs.size()) {
            logger_.warn("setTrack failed. Track not found in selected source: " + trackBasename);
            return false;
        }

        const bool wasPlaying = state_ == PlaybackState::Playing;
        if (state_ == PlaybackState::Playing || state_ == PlaybackState::Paused) {
            stopPlaybackDeviceLocked(true);
            state_ = PlaybackState::Stopped;
            trackStartValid_ = false;
            lastVolume_ = -1;
            lastLeftVolume_ = -1;
            lastRightVolume_ = -1;
        }

        songIndex_ = foundIndex;
        currentTrackPath_ = channel.songs[foundIndex];
        mode_ = channel.type == ChannelType::Station ? PlaybackMode::Station : PlaybackMode::Playlist;
        previousWasSong_ = true;

        logger_.info("setTrack selected: " + pathToUtf8(currentTrackPath_.filename()) +
                     " (index=" + std::to_string(foundIndex) + ")");

        if (!wasPlaying) {
            return true;
        }

        return playPathLocked(currentTrackPath_);
    });
}

bool RadioEngine::playFx(const std::string& fxBasename, std::uint64_t deviceId)
{
    return runBoolCommandForDevice(deviceId, [this, fxBasename]() {
        const auto fxPath = findFxPathLocked(fxBasename);
        if (!fxPath.has_value()) {
            logger_.warn("playFx failed. FX file not found: " + fxBasename);
            return false;
        }
        return playFxLocked(*fxPath);
    });
}

bool RadioEngine::stopFx(std::uint64_t deviceId)
{
    return runBoolCommandForDevice(deviceId, [this]() {
        stopFxLocked();
        return true;
    });
}

void RadioEngine::requestPlayInterrupt(std::uint64_t)
{
    playInterruptRequested_.store(true, std::memory_order_release);
}

bool RadioEngine::playAsync(
    std::uint64_t deviceId,
    const std::function<void(bool result)>& completion)
{
    return runAsyncCommandForDevice(deviceId, [this]() {
        clearPlayInterruptRequest();

        if (selectedKey_.empty()) {
            logger_.warn("play failed. No channel selected.");
            return false;
        }

        if (state_ == PlaybackState::Paused) {
            return resumeLocked();
        }

        const auto channelIt = channels_.find(selectedKey_);
        if (channelIt == channels_.end()) {
            logger_.warn("play failed. Selected channel no longer exists.");
            return false;
        }

        const PlaybackMode desiredMode =
            channelIt->second.type == ChannelType::Station ? PlaybackMode::Station : PlaybackMode::Playlist;

        return startCurrentLocked(desiredMode, false);
    }, completion);
}

bool RadioEngine::isPlayInterruptRequested() const
{
    return playInterruptRequested_.load(std::memory_order_acquire);
}

void RadioEngine::clearPlayInterruptRequest()
{
    playInterruptRequested_.store(false, std::memory_order_release);
}

std::string RadioEngine::currentChannel(std::uint64_t deviceId) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (deviceId == currentDeviceId_) {
        return selectedKey_;
    }

    const auto it = deviceStates_.find(deviceId);
    if (it == deviceStates_.end()) {
        return {};
    }
    return it->second.selectedKey;
}

std::string RadioEngine::currentSourceName(std::uint64_t deviceId) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string key = (deviceId == currentDeviceId_)
        ? selectedKey_
        : [this, deviceId]() -> std::string {
              const auto stateIt = deviceStates_.find(deviceId);
              return stateIt != deviceStates_.end() ? stateIt->second.selectedKey : std::string{};
          }();

    if (key.empty()) {
        return {};
    }

    const auto it = channels_.find(key);
    if (it == channels_.end()) {
        return {};
    }

    return it->second.displayName;
}

std::string RadioEngine::currentTrackBasename(std::uint64_t deviceId) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (deviceId == currentDeviceId_) {
        if (state_ != PlaybackState::Playing && state_ != PlaybackState::Paused) {
            return {};
        }
        if (currentTrackPath_.empty()) {
            return {};
        }
        return pathToUtf8(currentTrackPath_.filename());
    }

    const auto it = deviceStates_.find(deviceId);
    if (it == deviceStates_.end()) {
        return {};
    }
    if (it->second.state != PlaybackState::Playing && it->second.state != PlaybackState::Paused) {
        return {};
    }
    if (it->second.currentTrackPath.empty()) {
        return {};
    }
    return pathToUtf8(it->second.currentTrackPath.filename());
}

std::size_t RadioEngine::channelCount() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return channels_.size();
}

bool RadioEngine::loadConfig()
{
    config_.radioRootPath = defaultRadioRoot();
    config_.streamStations.clear();

    const auto path = configPath();
    if (!std::filesystem::exists(path)) {
        logger_.warn("Config not found at " + pathToUtf8(path) + ". Using defaults.");
        return false;
    }

    std::ifstream in(path);
    if (!in.is_open()) {
        logger_.warn("Could not open config file: " + pathToUtf8(path));
        return false;
    }

    std::string line;
    while (std::getline(in, line)) {
        const auto commentPos = line.find_first_of("#;");
        if (commentPos != std::string::npos) {
            line = line.substr(0, commentPos);
        }
        line = trim(line);
        if (line.empty()) {
            continue;
        }

        const auto eqPos = line.find('=');
        if (eqPos == std::string::npos) {
            continue;
        }

        std::string key = toLower(trim(line.substr(0, eqPos)));
        std::string value = trim(line.substr(eqPos + 1));
        if (value.empty()) {
            continue;
        }

        try {
            if (key == "root_path") {
                config_.radioRootPath = expandWindowsEnvironmentVariables(value);
            } else if (key == "log_level") {
                if (!logger_.setLevelFromString(value)) {
                    logger_.warn("Invalid config value for key: log_level (" + value + ")");
                }
            } else if (key == "transition_prefix") {
                config_.transitionPrefix = value;
            } else if (key == "ad_prefix") {
                config_.adPrefix = value;
            } else if (key == "ad_interval_songs") {
                config_.adIntervalSongs = static_cast<std::size_t>(std::max(1, std::stoi(value)));
            } else if (key == "min_fade_distance") {
                config_.minFadeDistance = std::stof(value);
            } else if (key == "max_fade_distance") {
                config_.maxFadeDistance = std::stof(value);
            } else if (key == "enable_spatial_pan") {
                config_.enableSpatialPan = value == "1" || toLower(value) == "true";
            } else if (key == "pan_distance") {
                config_.panDistance = std::stof(value);
            } else if (key == "log_fade_changes") {
                config_.logFadeChanges = value == "1" || toLower(value) == "true";
            } else if (key == "auto_rescan_on_change_playlist") {
                config_.autoRescanOnChangePlaylist = value == "1" || toLower(value) == "true";
            } else if (key == "loop_playlist") {
                config_.loopPlaylist = value == "1" || toLower(value) == "true";
            } else if (key == "verbose_stream_diagnostics") {
                config_.verboseStreamDiagnostics = value == "1" || toLower(value) == "true";
            } else if (key == "stream_station") {
                const auto sep = value.find('|');
                if (sep == std::string::npos) {
                    logger_.warn("Invalid stream_station entry, expected Name|Url: " + value);
                } else {
                    const std::string name = trim(value.substr(0, sep));
                    const std::string url = trim(value.substr(sep + 1));
                    if (name.empty() || url.empty()) {
                        logger_.warn("Invalid stream_station entry, empty name/url: " + value);
                    } else {
                        config_.streamStations.emplace_back(name, url);
                    }
                }
            }
        } catch (...) {
            logger_.warn("Invalid config value for key: " + key + " (" + value + ")");
        }
    }

    if (config_.maxFadeDistance < config_.minFadeDistance + kMinimumFadeGap) {
        config_.maxFadeDistance = config_.minFadeDistance + kMinimumFadeGap;
    }
    if (config_.panDistance < kMinimumFadeGap) {
        config_.panDistance = kMinimumFadeGap;
    }

    logger_.info("Config loaded. root_path=" + pathToUtf8(config_.radioRootPath) +
                 ", spatial_pan=" + std::string(config_.enableSpatialPan ? "true" : "false") +
                 ", pan_distance=" + std::to_string(config_.panDistance));
    return true;
}

std::filesystem::path RadioEngine::configPath()
{
    return std::filesystem::path("Data") / "SFSE" / "Plugins" / "RadioSFSE.ini";
}

std::string RadioEngine::trim(const std::string& text)
{
    std::size_t start = 0;
    while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start])) != 0) {
        ++start;
    }

    std::size_t end = text.size();
    while (end > start && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) {
        --end;
    }

    return text.substr(start, end - start);
}

std::string RadioEngine::toLower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return text;
}

bool RadioEngine::hasAudioExtension(const std::filesystem::path& path)
{
    const std::string ext = toLower(pathToUtf8(path.extension()));
    return ext == ".mp3" || ext == ".wav" || ext == ".ogg" || ext == ".flac";
}

std::wstring RadioEngine::utf8ToWide(const std::string& text)
{
    if (text.empty()) {
        return {};
    }

    const int needed = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
    if (needed <= 0) {
        return {};
    }

    std::wstring out(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), needed);
    return out;
}

std::string RadioEngine::wideToUtf8(const std::wstring& text)
{
    if (text.empty()) {
        return {};
    }

    const int needed = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        return {};
    }

    std::string out(static_cast<std::size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), needed, nullptr, nullptr);
    return out;
}

std::wstring RadioEngine::quoteForMCI(const std::filesystem::path& path)
{
    std::wstring wide = path.wstring();
    std::replace(wide.begin(), wide.end(), L'\"', L'\'');
    return L"\"" + wide + L"\"";
}

std::wstring RadioEngine::quoteForMCI(const std::wstring& text)
{
    std::wstring wide = text;
    std::replace(wide.begin(), wide.end(), L'\"', L'\'');
    return L"\"" + wide + L"\"";
}

bool RadioEngine::scanLibraryLocked()
{
    channels_.clear();
    fxFiles_.clear();
    streamOrderKeys_.clear();

    const std::string transitionPrefixLower = toLower(config_.transitionPrefix);
    const std::string adPrefixLower = toLower(config_.adPrefix);

    const auto scanCategory = [this, &transitionPrefixLower, &adPrefixLower](
                                  const std::filesystem::path& categoryRoot,
                                  const char* keyPrefix,
                                  ChannelType channelType) {
        if (!std::filesystem::exists(categoryRoot)) {
            logger_.warn("Category root path does not exist: " + pathToUtf8(categoryRoot));
            return;
        }
        if (!std::filesystem::is_directory(categoryRoot)) {
            logger_.warn("Category root is not a directory: " + pathToUtf8(categoryRoot));
            return;
        }

        std::error_code ec;
        for (std::filesystem::directory_iterator sourceIt(categoryRoot, ec), sourceEnd; sourceIt != sourceEnd && !ec; sourceIt.increment(ec)) {
            if (!sourceIt->is_directory()) {
                continue;
            }

            const auto sourcePath = sourceIt->path();
            const std::string sourceName = pathToUtf8(sourcePath.filename());
            if (sourceName.empty()) {
                continue;
            }

            std::vector<std::filesystem::path> songs;
            std::vector<std::filesystem::path> transitions;
            std::vector<std::filesystem::path> ads;

            std::error_code fileEc;
            for (std::filesystem::directory_iterator fileIt(sourcePath, fileEc), fileEnd; fileIt != fileEnd && !fileEc; fileIt.increment(fileEc)) {
                if (!fileIt->is_regular_file()) {
                    continue;
                }

                const auto filePath = fileIt->path();
                if (!hasAudioExtension(filePath)) {
                    continue;
                }

                if (channelType == ChannelType::Playlist) {
                    songs.push_back(filePath);
                    continue;
                }

                const std::string stemLower = toLower(pathToUtf8(filePath.stem()));
                if (!transitionPrefixLower.empty() && stemLower.starts_with(transitionPrefixLower)) {
                    transitions.push_back(filePath);
                } else if (!adPrefixLower.empty() && stemLower.starts_with(adPrefixLower)) {
                    ads.push_back(filePath);
                } else {
                    songs.push_back(filePath);
                }
            }

            if (songs.empty()) {
                continue;
            }

            std::sort(songs.begin(), songs.end());
            std::sort(transitions.begin(), transitions.end());
            std::sort(ads.begin(), ads.end());

            const std::string key = std::string(keyPrefix) + "/" + toLower(sourceName);
            ChannelEntry entry;
            entry.key = key;
            entry.displayName = sourceName;
            entry.directoryPath = sourcePath;
            entry.type = channelType;
            entry.songs = std::move(songs);
            entry.transitions = std::move(transitions);
            entry.ads = std::move(ads);

            channels_[key] = std::move(entry);
        }
    };

    if (config_.radioRootPath.empty() || !std::filesystem::exists(config_.radioRootPath)) {
        logger_.warn("Radio root path does not exist: " + pathToUtf8(config_.radioRootPath));
    } else {
        scanCategory(config_.radioRootPath / "Playlists", "playlist", ChannelType::Playlist);
        scanCategory(config_.radioRootPath / "Stations", "station", ChannelType::Station);
    }

    addConfiguredStreamsLocked();

    const std::filesystem::path fxRoot = config_.radioRootPath / "FX";
    if (std::filesystem::exists(fxRoot) && std::filesystem::is_directory(fxRoot)) {
        std::error_code fxEc;
        for (std::filesystem::directory_iterator fxIt(fxRoot, fxEc), fxEnd; fxIt != fxEnd && !fxEc; fxIt.increment(fxEc)) {
            if (!fxIt->is_regular_file()) {
                continue;
            }

            const auto filePath = fxIt->path();
            if (!hasAudioExtension(filePath)) {
                continue;
            }

            const std::string fileNameLower = toLower(pathToUtf8(filePath.filename()));
            const std::string stemLower = toLower(pathToUtf8(filePath.stem()));
            if (!fileNameLower.empty() && !fxFiles_.contains(fileNameLower)) {
                fxFiles_[fileNameLower] = filePath;
            }
            if (!stemLower.empty() && !fxFiles_.contains(stemLower)) {
                fxFiles_[stemLower] = filePath;
            }
        }
    }

    return !channels_.empty();
}

void RadioEngine::addConfiguredStreamsLocked()
{
    for (const auto& [name, url] : config_.streamStations) {
        const std::string key = "stream/" + toLower(trim(name));
        if (key.empty() || url.empty()) {
            continue;
        }

        ChannelEntry entry;
        entry.key = key;
        entry.displayName = name;
        entry.type = ChannelType::Station;
        entry.isStream = true;
        entry.streamUrl = url;

        if (std::find(streamOrderKeys_.begin(), streamOrderKeys_.end(), key) == streamOrderKeys_.end()) {
            streamOrderKeys_.push_back(key);
        }
        channels_[key] = std::move(entry);
    }
}

std::optional<RadioEngine::ChannelEntry> RadioEngine::lookupChannelLocked(const std::string& channelName) const
{
    std::string key = toLower(trim(channelName));
    if (key.empty()) {
        return std::nullopt;
    }

    if (key.starts_with("playlists/")) {
        key = "playlist/" + key.substr(std::string("playlists/").size());
    } else if (key.starts_with("stations/")) {
        key = "station/" + key.substr(std::string("stations/").size());
    }

    auto it = channels_.find(key);
    if (it != channels_.end()) {
        return it->second;
    }

    std::vector<std::string> prefixedKeys = {
        "playlist/" + key,
        "station/" + key,
        "stream/" + key
    };

    std::optional<ChannelEntry> uniqueMatch;
    for (const auto& candidate : prefixedKeys) {
        const auto byPrefix = channels_.find(candidate);
        if (byPrefix == channels_.end()) {
            continue;
        }
        if (uniqueMatch.has_value()) {
            return std::nullopt;
        }
        uniqueMatch = byPrefix->second;
    }
    if (uniqueMatch.has_value()) {
        return uniqueMatch;
    }

    for (const auto& [mapKey, entry] : channels_) {
        (void)mapKey;
        if (toLower(entry.displayName) == key) {
            return entry;
        }
    }

    return std::nullopt;
}

bool RadioEngine::startCurrentLocked(PlaybackMode mode, bool resetPosition)
{
    const auto channelIt = channels_.find(selectedKey_);
    if (channelIt == channels_.end()) {
        logger_.warn("startCurrent failed. Selected channel no longer exists.");
        return false;
    }

    const auto& channel = channelIt->second;
    if (channel.songs.empty()) {
        if (!channel.isStream) {
            logger_.warn("startCurrent failed. Channel has no songs: " + channel.displayName);
            return false;
        }
    }

    mode_ = mode;
    if (resetPosition) {
        songIndex_ = 0;
        transitionIndex_ = 0;
        adIndex_ = 0;
        songsSinceAd_ = 0;
    }

    if (mode_ == PlaybackMode::Station) {
        previousWasSong_ = true;
    }

    if (channel.isStream) {
        return playStreamLocked(channel.streamUrl);
    }

    const auto track = chooseCurrentTrackLocked();
    if (!track.has_value()) {
        logger_.warn("startCurrent failed. Could not determine track.");
        return false;
    }

    return playPathLocked(*track);
}

bool RadioEngine::playPathLocked(const std::filesystem::path& filePath)
{
    stopPlaybackDeviceLocked(true);
    if (!waitForAliasClosedLocked(std::chrono::milliseconds(150))) {
        logger_.warn("MCI alias still open before file play. Attempting reopen anyway.");
    }

    const std::wstring quotedPath = quoteForMCI(filePath);
    bool opened = mciCommandLocked(L"open " + quotedPath + L" alias " + kAlias);
    if (!opened) {
        opened = mciCommandLocked(L"open " + quotedPath + L" type mpegvideo alias " + kAlias);
    }
    if (!opened) {
        logger_.warn("MCI local open failed. Trying Media Foundation fallback for: " + pathToUtf8(filePath));
        if (!startMediaFoundationStreamLocked(pathToUtf8(filePath), true)) {
            state_ = PlaybackState::Stopped;
            currentTrackPath_.clear();
            lastVolume_ = -1;
            lastLeftVolume_ = -1;
            lastRightVolume_ = -1;
            trackStartValid_ = false;
            return false;
        }

        currentTrackPath_ = filePath;
        backend_ = PlaybackBackend::MediaFoundationStream;
        state_ = PlaybackState::Playing;
        trackStartTime_ = std::chrono::steady_clock::now();
        trackStartValid_ = true;
        stopFxLocked();
        updateFadeVolumeLocked();
        logger_.info("Now playing (Media Foundation fallback): " + pathToUtf8(filePath));
        return true;
    }

    (void)mciCommandLocked(L"set " + std::wstring(kAlias) + L" time format milliseconds");
    if (!mciCommandLocked(L"play " + std::wstring(kAlias))) {
        (void)mciCommandLocked(L"close " + std::wstring(kAlias));
        state_ = PlaybackState::Stopped;
        currentTrackPath_.clear();
        lastVolume_ = -1;
        lastLeftVolume_ = -1;
        lastRightVolume_ = -1;
        trackStartValid_ = false;
        return false;
    }

    currentTrackPath_ = filePath;
    backend_ = PlaybackBackend::MCI;
    state_ = PlaybackState::Playing;
    trackStartTime_ = std::chrono::steady_clock::now();
    trackStartValid_ = true;
    stopFxLocked();
    updateFadeVolumeLocked();

    logger_.info("Now playing: " + pathToUtf8(filePath));
    return true;
}

bool RadioEngine::playFxLocked(const std::filesystem::path& filePath)
{
    stopFxLocked();

    const std::wstring quotedPath = quoteForMCI(filePath);
    bool opened = mciCommandLocked(L"open " + quotedPath + L" alias " + kFxAlias);
    if (!opened) {
        opened = mciCommandLocked(L"open " + quotedPath + L" type mpegvideo alias " + kFxAlias);
    }
    if (!opened) {
        return false;
    }

    if (!mciCommandLocked(L"play " + std::wstring(kFxAlias))) {
        (void)mciCommandLocked(L"close " + std::wstring(kFxAlias));
        return false;
    }

    return true;
}

void RadioEngine::stopFxLocked()
{
    (void)mciSendStringW((L"stop " + std::wstring(kFxAlias)).c_str(), nullptr, 0, nullptr);
    (void)mciSendStringW((L"close " + std::wstring(kFxAlias)).c_str(), nullptr, 0, nullptr);
}

std::optional<std::filesystem::path> RadioEngine::findFxPathLocked(const std::string& fxBasename)
{
    const std::string key = toLower(trim(fxBasename));
    if (key.empty()) {
        return std::nullopt;
    }

    auto it = fxFiles_.find(key);
    if (it != fxFiles_.end()) {
        return it->second;
    }

    if (config_.autoRescanOnChangePlaylist) {
        (void)scanLibraryLocked();
        it = fxFiles_.find(key);
        if (it != fxFiles_.end()) {
            return it->second;
        }
    }

    return std::nullopt;
}

bool RadioEngine::ensureMediaFoundationLocked()
{
    if (!mfState_) {
        mfState_ = std::make_unique<MfState>();
    }

    if (!mfState_->comInitialized) {
        const HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (SUCCEEDED(coHr)) {
            mfState_->comInitialized = true;
            mfState_->ownsComInitialization = true;
        } else if (coHr == RPC_E_CHANGED_MODE) {
            mfState_->comInitialized = true;
            mfState_->ownsComInitialization = false;
        } else {
            logger_.warn("Media Foundation unavailable: CoInitializeEx failed: " + formatHresult(coHr));
            return false;
        }
    }

    if (!mfState_->mfInitialized) {
        const HRESULT mfHr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
        if (FAILED(mfHr)) {
            logger_.warn("Media Foundation startup failed: " + formatHresult(mfHr));
            return false;
        }
        mfState_->mfInitialized = true;
    }

    if (!mfState_->callback) {
        mfState_->events = std::make_shared<MfEventState>();
        mfState_->callback.Attach(new MfPlayerCallback(mfState_->events));
    }

    return true;
}

bool RadioEngine::startMediaFoundationStreamLocked(const std::string& streamUrl, bool detailedLogs)
{
    if (isPlayInterruptRequested()) {
        return false;
    }

    if (!ensureMediaFoundationLocked()) {
        return false;
    }
    if (!mfState_ || !mfState_->callback) {
        return false;
    }

    if (mfState_->player) {
        (void)mfState_->player->Stop();
        (void)mfState_->player->Shutdown();
        mfState_->player.Reset();
    }

    mfState_->events->lastError.store(S_OK);
    mfState_->events->playbackEnded.store(false);

    const std::wstring wideUrl = utf8ToWide(streamUrl);
    if (wideUrl.empty()) {
        if (detailedLogs) {
            logger_.warn("Media Foundation stream open failed: URL conversion to UTF-16 returned empty.");
        }
        return false;
    }

    const HRESULT createHr = MFPCreateMediaPlayer(
        wideUrl.c_str(),
        FALSE,
        MFP_OPTION_FREE_THREADED_CALLBACK,
        mfState_->callback.Get(),
        nullptr,
        mfState_->player.ReleaseAndGetAddressOf());
    if (FAILED(createHr) || !mfState_->player) {
        if (detailedLogs) {
            logger_.warn("Media Foundation could not open stream: " + streamUrl +
                         " | hr=" + formatHresult(createHr));
        }
        return false;
    }

    const HRESULT playHr = mfState_->player->Play();
    if (FAILED(playHr)) {
        if (detailedLogs) {
            logger_.warn("Media Foundation Play failed for stream: " + streamUrl +
                         " | hr=" + formatHresult(playHr));
        }
        (void)mfState_->player->Shutdown();
        mfState_->player.Reset();
        return false;
    }

    const auto deadline = std::chrono::steady_clock::now() + kStreamStartWaitTimeout;
    MFP_MEDIAPLAYER_STATE lastState = MFP_MEDIAPLAYER_STATE_EMPTY;
    while (std::chrono::steady_clock::now() < deadline) {
        if (isPlayInterruptRequested()) {
            (void)mfState_->player->Stop();
            (void)mfState_->player->Shutdown();
            mfState_->player.Reset();
            return false;
        }

        const HRESULT asyncHr = mfState_->events->lastError.load();
        if (FAILED(asyncHr)) {
            if (detailedLogs) {
                logger_.warn("Media Foundation stream error: " + streamUrl +
                             " | hr=" + formatHresult(asyncHr));
            }
            (void)mfState_->player->Stop();
            (void)mfState_->player->Shutdown();
            mfState_->player.Reset();
            return false;
        }

        MFP_MEDIAPLAYER_STATE playerState = MFP_MEDIAPLAYER_STATE_EMPTY;
        const HRESULT stateHr = mfState_->player->GetState(&playerState);
        if (SUCCEEDED(stateHr)) {
            lastState = playerState;
            if (playerState == MFP_MEDIAPLAYER_STATE_PLAYING || playerState == MFP_MEDIAPLAYER_STATE_PAUSED) {
                return true;
            }
            if (playerState == MFP_MEDIAPLAYER_STATE_SHUTDOWN) {
                if (detailedLogs) {
                    logger_.warn("Media Foundation stream state is shutdown: " + streamUrl);
                }
                (void)mfState_->player->Shutdown();
                mfState_->player.Reset();
                return false;
            }
        }

        std::this_thread::sleep_for(kStreamStartPoll);
    }

    MFP_MEDIAPLAYER_STATE finalState = MFP_MEDIAPLAYER_STATE_EMPTY;
    const HRESULT finalStateHr = mfState_->player->GetState(&finalState);
    const HRESULT finalAsyncHr = mfState_->events->lastError.load();
    if (SUCCEEDED(finalStateHr) &&
        (finalState == MFP_MEDIAPLAYER_STATE_PLAYING || finalState == MFP_MEDIAPLAYER_STATE_PAUSED)) {
        return true;
    }

    if (SUCCEEDED(finalStateHr)) {
        lastState = finalState;
    }
    if (detailedLogs) {
        logger_.warn("Media Foundation stream did not enter PLAYING state in time: " + streamUrl +
                     " | state=" + mfStateName(lastState) +
                     " | lastHr=" + formatHresult(finalAsyncHr));
    }
    (void)mfState_->player->Stop();
    (void)mfState_->player->Shutdown();
    mfState_->player.Reset();
    return false;
}

void RadioEngine::shutdownMediaFoundationLocked()
{
    if (!mfState_) {
        return;
    }

    if (mfState_->player) {
        (void)mfState_->player->Stop();
        (void)mfState_->player->Shutdown();
        mfState_->player.Reset();
    }

    mfState_->callback.Reset();
    mfState_->events.reset();

    if (mfState_->mfInitialized) {
        (void)MFShutdown();
        mfState_->mfInitialized = false;
    }

    if (mfState_->comInitialized && mfState_->ownsComInitialization) {
        CoUninitialize();
        mfState_->comInitialized = false;
        mfState_->ownsComInitialization = false;
    }
}

bool RadioEngine::startDirectShowStreamLocked(const std::string& streamUrl, bool detailedLogs)
{
    if (isPlayInterruptRequested()) {
        return false;
    }

    HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool needsUninit = SUCCEEDED(coHr);
    if (coHr == RPC_E_CHANGED_MODE) {
        coHr = S_OK;
    }
    if (FAILED(coHr)) {
        if (detailedLogs) {
            logger_.warn("DirectShow unavailable: CoInitializeEx failed: " + formatHresult(coHr));
        }
        return false;
    }

    if (!dsState_) {
        dsState_ = std::make_unique<DsState>();
    } else {
        shutdownDirectShowLocked();
    }

    auto cleanupOnFailure = [this, needsUninit]() {
        shutdownDirectShowLocked();
        if (needsUninit) {
            CoUninitialize();
        }
    };

    const HRESULT graphHr = CoCreateInstance(
        CLSID_FilterGraph,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_IGraphBuilder,
        reinterpret_cast<void**>(dsState_->graph.ReleaseAndGetAddressOf()));
    if (FAILED(graphHr) || !dsState_->graph) {
        if (detailedLogs) {
            logger_.warn("DirectShow graph creation failed for stream: " + streamUrl +
                         " | hr=" + formatHresult(graphHr));
        }
        cleanupOnFailure();
        return false;
    }

    const HRESULT controlHr = dsState_->graph.As(&dsState_->control);
    const HRESULT eventHr = dsState_->graph.As(&dsState_->events);
    (void)dsState_->graph.As(&dsState_->audio);
    if (FAILED(controlHr) || FAILED(eventHr) || !dsState_->control || !dsState_->events) {
        if (detailedLogs) {
            logger_.warn("DirectShow interface query failed for stream: " + streamUrl +
                         " | controlHr=" + formatHresult(controlHr) +
                         " | eventHr=" + formatHresult(eventHr));
        }
        cleanupOnFailure();
        return false;
    }

    const std::wstring wideUrl = utf8ToWide(streamUrl);
    if (wideUrl.empty()) {
        if (detailedLogs) {
            logger_.warn("DirectShow stream open failed: URL conversion to UTF-16 returned empty.");
        }
        cleanupOnFailure();
        return false;
    }

    if (isPlayInterruptRequested()) {
        cleanupOnFailure();
        return false;
    }

    const HRESULT renderHr = dsState_->graph->RenderFile(wideUrl.c_str(), nullptr);
    if (FAILED(renderHr)) {
        if (detailedLogs) {
            logger_.warn("DirectShow could not render stream: " + streamUrl +
                         " | hr=" + formatHresult(renderHr));
        }
        cleanupOnFailure();
        return false;
    }

    if (isPlayInterruptRequested()) {
        cleanupOnFailure();
        return false;
    }

    const HRESULT runHr = dsState_->control->Run();
    if (FAILED(runHr)) {
        if (detailedLogs) {
            logger_.warn("DirectShow could not run stream: " + streamUrl +
                         " | hr=" + formatHresult(runHr));
        }
        cleanupOnFailure();
        return false;
    }

    if (needsUninit) {
        // Keep COM initialized for this thread lifetime; matching uninit happens on shutdown.
        if (mfState_) {
            mfState_->comInitialized = true;
            mfState_->ownsComInitialization = true;
        }
    }

    return true;
}

void RadioEngine::shutdownDirectShowLocked()
{
    if (!dsState_) {
        return;
    }

    if (dsState_->control) {
        (void)dsState_->control->Stop();
    }
    dsState_->audio.Reset();
    dsState_->events.Reset();
    dsState_->control.Reset();
    dsState_->graph.Reset();
}

bool RadioEngine::playStreamLocked(const std::string& streamUrl)
{
    stopPlaybackDeviceLocked(true);

    const std::string directUrl = trim(streamUrl);
    if (directUrl.empty()) {
        logger_.warn("Stream play failed: empty URL.");
        state_ = PlaybackState::Stopped;
        currentTrackPath_.clear();
        lastVolume_ = -1;
        lastLeftVolume_ = -1;
        lastRightVolume_ = -1;
        trackStartValid_ = false;
        return false;
    }

    auto clearStreamState = [this]() {
        backend_ = PlaybackBackend::None;
        state_ = PlaybackState::Stopped;
        currentTrackPath_.clear();
        lastVolume_ = -1;
        lastLeftVolume_ = -1;
        lastRightVolume_ = -1;
        trackStartValid_ = false;
    };

    std::vector<std::string> candidates;
    auto addCandidate = [&candidates](const std::string& url) {
        const std::string trimmedUrl = trimAsciiCopy(url);
        if (trimmedUrl.empty()) {
            return;
        }
        const std::string lowered = toLowerCopy(trimmedUrl);
        for (const auto& existing : candidates) {
            if (toLowerCopy(existing) == lowered) {
                return;
            }
        }
        candidates.push_back(trimmedUrl);
    };

    const std::string resolvedUrl = resolvePlayableStreamUrl(
        directUrl,
        logger_,
        0,
        [this]() {
            return isPlayInterruptRequested();
        });
    if (isPlayInterruptRequested()) {
        clearStreamState();
        return false;
    }
    const bool directLooksWrapper = isLikelyWrapperExtension(urlExtensionLower(directUrl));
    const bool hasResolvedAlternative =
        !resolvedUrl.empty() && toLower(resolvedUrl) != toLower(directUrl);

    auto addCandidateFamily = [&addCandidate](const std::string& baseUrl) {
        addCandidate(baseUrl);
        addCandidate(httpsToHttpVariant(baseUrl));
        addCandidate(httpToHttpsVariant(baseUrl));

        if (isLikelyWrapperExtension(urlExtensionLower(baseUrl))) {
            return;
        }
        for (const auto& variant : makeShoutcastStyleVariants(baseUrl)) {
            addCandidate(variant);
            addCandidate(httpsToHttpVariant(variant));
            addCandidate(httpToHttpsVariant(variant));
        }
    };

    if (!directLooksWrapper || !hasResolvedAlternative) {
        addCandidateFamily(directUrl);
    }
    if (hasResolvedAlternative) {
        addCandidateFamily(resolvedUrl);
    }
    if (candidates.empty()) {
        addCandidate(directUrl);
    }

    for (const auto& candidate : candidates) {
        if (isPlayInterruptRequested()) {
            clearStreamState();
            return false;
        }

        if (startMediaFoundationStreamLocked(candidate, config_.verboseStreamDiagnostics)) {
            streamWrapperTempPath_.clear();
            currentTrackPath_.clear();
            backend_ = PlaybackBackend::MediaFoundationStream;
            state_ = PlaybackState::Playing;
            trackStartTime_ = std::chrono::steady_clock::now();
            trackStartValid_ = true;
            stopFxLocked();
            updateFadeVolumeLocked();
            if (candidate == directUrl) {
                logger_.info("Now streaming: " + directUrl);
            } else {
                logger_.info("Now streaming: " + directUrl + " (resolved: " + candidate + ")");
            }
            return true;
        }

        if (isPlayInterruptRequested()) {
            clearStreamState();
            return false;
        }

        if (startDirectShowStreamLocked(candidate, config_.verboseStreamDiagnostics)) {
            streamWrapperTempPath_.clear();
            currentTrackPath_.clear();
            backend_ = PlaybackBackend::DirectShowStream;
            state_ = PlaybackState::Playing;
            trackStartTime_ = std::chrono::steady_clock::now();
            trackStartValid_ = true;
            stopFxLocked();
            updateFadeVolumeLocked();
            if (candidate == directUrl) {
                logger_.info("Now streaming (DirectShow fallback): " + directUrl);
            } else {
                logger_.info("Now streaming (DirectShow fallback): " + directUrl + " (resolved: " + candidate + ")");
            }
            return true;
        }
    }

    clearStreamState();
    logger_.warn("Stream play failed after all URL attempts: " + directUrl);
    return false;
}

void RadioEngine::stopPlaybackDeviceLocked(bool closeDevice)
{
    if (backend_ == PlaybackBackend::MediaFoundationStream && mfState_ && mfState_->player) {
        (void)mfState_->player->Stop();
        if (closeDevice) {
            (void)mfState_->player->Shutdown();
            mfState_->player.Reset();
            if (mfState_->events) {
                mfState_->events->lastError.store(S_OK);
                mfState_->events->playbackEnded.store(false);
            }
        }
    } else if (backend_ == PlaybackBackend::DirectShowStream && dsState_ && dsState_->control) {
        (void)dsState_->control->Stop();
        if (closeDevice) {
            shutdownDirectShowLocked();
        }
    } else {
        (void)mciSendStringW((L"stop " + std::wstring(kAlias)).c_str(), nullptr, 0, nullptr);
        if (closeDevice) {
            bool closed = false;
            for (int attempt = 0; attempt < 3 && !closed; ++attempt) {
                const MCIERROR closeErr =
                    mciSendStringW((L"close " + std::wstring(kAlias)).c_str(), nullptr, 0, nullptr);
                if (closeErr == 0) {
                    closed = waitForAliasClosedLocked(std::chrono::milliseconds(80));
                } else {
                    std::wstring mode;
                    if (!mciStatusModeSilentLocked(mode)) {
                        closed = true;
                    }
                }

                if (!closed) {
                    (void)mciSendStringW((L"stop " + std::wstring(kAlias)).c_str(), nullptr, 0, nullptr);
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                }
            }

            if (!closed) {
                logger_.warn("MCI alias did not close cleanly after retries.");
            }
        }
    }

    if (closeDevice) {
        backend_ = PlaybackBackend::None;
        cleanupCurrentStreamTempFileLocked();
    }
}

bool RadioEngine::resumeLocked()
{
    if (state_ != PlaybackState::Paused) {
        return false;
    }

    if (backend_ == PlaybackBackend::MediaFoundationStream) {
        if (!mfState_ || !mfState_->player) {
            logger_.warn("resume failed. Stream backend player is not available.");
            return false;
        }

        const HRESULT playHr = mfState_->player->Play();
        if (FAILED(playHr)) {
            logger_.warn("resume failed. Media Foundation Play failed: " + formatHresult(playHr));
            return false;
        }
    } else if (backend_ == PlaybackBackend::DirectShowStream) {
        if (!dsState_ || !dsState_->control) {
            logger_.warn("resume failed. DirectShow backend player is not available.");
            return false;
        }

        const HRESULT runHr = dsState_->control->Run();
        if (FAILED(runHr)) {
            logger_.warn("resume failed. DirectShow Run failed: " + formatHresult(runHr));
            return false;
        }
    } else {
        if (!mciCommandLocked(L"resume " + std::wstring(kAlias))) {
            if (!mciCommandLocked(L"play " + std::wstring(kAlias))) {
                logger_.warn("resume failed.");
                return false;
            }
        }
    }

    state_ = PlaybackState::Playing;
    logger_.info("Playback resumed.");
    return true;
}

bool RadioEngine::pauseLocked()
{
    if (state_ != PlaybackState::Playing) {
        return false;
    }

    if (backend_ == PlaybackBackend::MediaFoundationStream) {
        if (!mfState_ || !mfState_->player) {
            logger_.warn("pause failed. Stream backend player is not available.");
            return false;
        }

        const HRESULT pauseHr = mfState_->player->Pause();
        if (FAILED(pauseHr)) {
            logger_.warn("pause failed. Media Foundation Pause failed: " + formatHresult(pauseHr));
            return false;
        }
    } else if (backend_ == PlaybackBackend::DirectShowStream) {
        if (!dsState_ || !dsState_->control) {
            logger_.warn("pause failed. DirectShow backend player is not available.");
            return false;
        }

        const HRESULT pauseHr = dsState_->control->Pause();
        if (FAILED(pauseHr)) {
            logger_.warn("pause failed. DirectShow Pause failed: " + formatHresult(pauseHr));
            return false;
        }
    } else {
        if (!mciCommandLocked(L"pause " + std::wstring(kAlias))) {
            logger_.warn("pause failed.");
            return false;
        }
    }

    state_ = PlaybackState::Paused;
    logger_.info("Playback paused.");
    return true;
}

bool RadioEngine::forwardLocked()
{
    if (selectedKey_.empty()) {
        return false;
    }

    const auto channelIt = channels_.find(selectedKey_);
    if (channelIt == channels_.end()) {
        return false;
    }

    if (channelIt->second.isStream) {
        logger_.info("forward -> restart stream.");
        return playStreamLocked(channelIt->second.streamUrl);
    }

    if (channelIt->second.songs.empty()) {
        return false;
    }

    if (mode_ == PlaybackMode::Station || channelIt->second.type == ChannelType::Station) {
        songIndex_ = (songIndex_ + 1) % channelIt->second.songs.size();
        previousWasSong_ = true;
        mode_ = PlaybackMode::Station;
    } else {
        songIndex_ = (songIndex_ + 1) % channelIt->second.songs.size();
        mode_ = PlaybackMode::Playlist;
    }

    const auto track = chooseCurrentTrackLocked();
    if (!track.has_value()) {
        return false;
    }

    logger_.info("forward -> " + track->string());
    return playPathLocked(*track);
}

bool RadioEngine::rewindLocked()
{
    const auto channelIt = channels_.find(selectedKey_);
    if (channelIt != channels_.end() && channelIt->second.isStream) {
        logger_.info("rewind -> restart stream.");
        return playStreamLocked(channelIt->second.streamUrl);
    }

    int positionMs = 0;
    if (mciStatusNumberLocked(L"position", positionMs) && positionMs > 3000) {
        if (mciCommandLocked(L"seek " + std::wstring(kAlias) + L" to 0")) {
            if (state_ != PlaybackState::Paused) {
                (void)mciCommandLocked(L"play " + std::wstring(kAlias));
                state_ = PlaybackState::Playing;
            }
            logger_.info("rewind -> restart current track.");
            return true;
        }
    }

    if (channelIt == channels_.end() || channelIt->second.songs.empty()) {
        return false;
    }

    const auto songCount = channelIt->second.songs.size();
    songIndex_ = (songIndex_ == 0) ? (songCount - 1) : (songIndex_ - 1);
    previousWasSong_ = true;

    if (mode_ == PlaybackMode::None) {
        mode_ = channelIt->second.type == ChannelType::Station ? PlaybackMode::Station : PlaybackMode::Playlist;
    }

    const auto track = chooseCurrentTrackLocked();
    if (!track.has_value()) {
        return false;
    }

    logger_.info("rewind -> " + track->string());
    return playPathLocked(*track);
}

bool RadioEngine::updateTrackLocked(bool force)
{
    if (state_ != PlaybackState::Playing) {
        return false;
    }

    const auto channelIt = channels_.find(selectedKey_);
    if (channelIt == channels_.end()) {
        return false;
    }

    if (channelIt->second.isStream) {
        if (!force && !isTrackCompleteLocked()) {
            return true;
        }
        logger_.info("Stream ended/disconnected, reconnecting: " + channelIt->second.displayName);
        return playStreamLocked(channelIt->second.streamUrl);
    }

    if (!force && !isTrackCompleteLocked()) {
        return true;
    }

    const auto nextTrack = advanceAndChooseNextTrackLocked();
    if (!nextTrack.has_value()) {
        stopPlaybackDeviceLocked(true);
        state_ = PlaybackState::Stopped;
        currentTrackPath_.clear();
        lastVolume_ = -1;
        lastLeftVolume_ = -1;
        lastRightVolume_ = -1;
        trackStartValid_ = false;
        logger_.info("Playback reached end of queue.");
        return false;
    }

    return playPathLocked(*nextTrack);
}

bool RadioEngine::isTrackCompleteLocked()
{
    if (trackStartValid_) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - trackStartTime_);
        const auto minProbeTime = (backend_ == PlaybackBackend::MediaFoundationStream ||
                                   backend_ == PlaybackBackend::DirectShowStream)
            ? std::chrono::milliseconds(2500)
            : std::chrono::milliseconds(800);
        if (elapsed < minProbeTime) {
            return false;
        }
    }

    if (backend_ == PlaybackBackend::MediaFoundationStream) {
        if (!mfState_ || !mfState_->player) {
            logger_.warn("Media Foundation player missing while state=Playing. Treating stream as complete.");
            return true;
        }
        if (mfState_->events) {
            const HRESULT asyncHr = mfState_->events->lastError.load();
            if (FAILED(asyncHr)) {
                logger_.warn("Media Foundation stream error while playing: " + formatHresult(asyncHr));
                return true;
            }
            if (mfState_->events->playbackEnded.load()) {
                logger_.warn("Media Foundation stream playback ended.");
                return true;
            }
        }

        MFP_MEDIAPLAYER_STATE playerState = MFP_MEDIAPLAYER_STATE_EMPTY;
        const HRESULT stateHr = mfState_->player->GetState(&playerState);
        if (FAILED(stateHr)) {
            logger_.warn("Media Foundation GetState failed while playing: " + formatHresult(stateHr));
            return true;
        }

        return !(playerState == MFP_MEDIAPLAYER_STATE_PLAYING ||
                 playerState == MFP_MEDIAPLAYER_STATE_PAUSED);
    }

    if (backend_ == PlaybackBackend::DirectShowStream) {
        if (!dsState_ || !dsState_->control) {
            logger_.warn("DirectShow player missing while state=Playing. Treating stream as complete.");
            return true;
        }

        if (dsState_->events) {
            long eventCode = 0;
            LONG_PTR param1 = 0;
            LONG_PTR param2 = 0;
            while (SUCCEEDED(dsState_->events->GetEvent(&eventCode, &param1, &param2, 0))) {
                dsState_->events->FreeEventParams(eventCode, param1, param2);
                if (eventCode == EC_COMPLETE || eventCode == EC_ERRORABORT) {
                    logger_.warn("DirectShow stream event indicates completion/error: code=" + std::to_string(eventCode));
                    return true;
                }
            }
        }

        OAFilterState filterState = State_Stopped;
        const HRESULT stateHr = dsState_->control->GetState(0, &filterState);
        if (FAILED(stateHr) && stateHr != VFW_S_STATE_INTERMEDIATE) {
            logger_.warn("DirectShow GetState failed while playing: " + formatHresult(stateHr));
            return true;
        }
        if (stateHr == VFW_S_STATE_INTERMEDIATE) {
            return false;
        }
        return !(filterState == State_Running || filterState == State_Paused);
    }

    std::wstring mode;
    if (!mciStatusModeSilentLocked(mode)) {
        logger_.warn("MCI alias missing while state=Playing. Treating track as complete.");
        return true;
    }

    const std::string modeLower = toLower(wideToUtf8(mode));
    if (modeLower == "playing" || modeLower == "paused" ||
        modeLower == "seeking" || modeLower == "open" || modeLower == "not ready") {
        return false;
    }

    return true;
}

std::optional<std::filesystem::path> RadioEngine::chooseCurrentTrackLocked() const
{
    const auto channelIt = channels_.find(selectedKey_);
    if (channelIt == channels_.end()) {
        return std::nullopt;
    }

    const auto& channel = channelIt->second;
    if (channel.songs.empty()) {
        return std::nullopt;
    }

    if (mode_ == PlaybackMode::Playlist || channel.type == ChannelType::Playlist) {
        if (songIndex_ >= channel.songs.size()) {
            if (!config_.loopPlaylist) {
                return std::nullopt;
            }
            return channel.songs[0];
        }
        return channel.songs[songIndex_];
    }

    if (songIndex_ >= channel.songs.size()) {
        return channel.songs[0];
    }
    return channel.songs[songIndex_];
}

std::optional<std::filesystem::path> RadioEngine::advanceAndChooseNextTrackLocked()
{
    const auto channelIt = channels_.find(selectedKey_);
    if (channelIt == channels_.end()) {
        return std::nullopt;
    }

    const auto& channel = channelIt->second;
    if (channel.songs.empty()) {
        return std::nullopt;
    }

    if (mode_ == PlaybackMode::Playlist || channel.type == ChannelType::Playlist) {
        ++songIndex_;
        if (songIndex_ >= channel.songs.size()) {
            if (!config_.loopPlaylist) {
                return std::nullopt;
            }
            songIndex_ = 0;
        }
        return channel.songs[songIndex_];
    }

    if (previousWasSong_) {
        ++songsSinceAd_;
        const bool playAd = !channel.ads.empty() &&
                            config_.adIntervalSongs > 0 &&
                            songsSinceAd_ >= config_.adIntervalSongs;
        if (playAd) {
            songsSinceAd_ = 0;
            const std::filesystem::path next = channel.ads[adIndex_ % channel.ads.size()];
            ++adIndex_;
            previousWasSong_ = false;
            return next;
        }

        if (!channel.transitions.empty()) {
            const std::filesystem::path next = channel.transitions[transitionIndex_ % channel.transitions.size()];
            ++transitionIndex_;
            previousWasSong_ = false;
            return next;
        }

        songIndex_ = (songIndex_ + 1) % channel.songs.size();
        previousWasSong_ = true;
        return channel.songs[songIndex_];
    }

    songIndex_ = (songIndex_ + 1) % channel.songs.size();
    previousWasSong_ = true;
    return channel.songs[songIndex_];
}

void RadioEngine::updateFadeVolumeLocked()
{
    if (state_ != PlaybackState::Playing) {
        return;
    }

    DeviceState& device = ensureDeviceStateLocked(currentDeviceId_);
    const double distance = distanceLocked();
    const float minDist = device.fadeOverride.enabled ? device.fadeOverride.minDistance : config_.minFadeDistance;
    const float maxDist = device.fadeOverride.enabled ? device.fadeOverride.maxDistance : config_.maxFadeDistance;
    const float panDist = device.fadeOverride.enabled ? device.fadeOverride.panDistance : config_.panDistance;

    double factor = 0.0;
    if (distance <= minDist) {
        factor = 1.0;
    } else if (distance >= maxDist) {
        factor = 0.0;
    } else {
        const double t = (distance - minDist) / (maxDist - minDist);
        factor = 1.0 - t;
        factor *= factor;
    }

    const double gain = std::clamp(static_cast<double>(device.volumeGain), 0.0, 2.0);
    const int volume = static_cast<int>(std::lround(std::clamp(factor * gain, 0.0, 1.0) * 1000.0));

    double pan = 0.0;
    int leftVolume = volume;
    int rightVolume = volume;
    if (config_.enableSpatialPan && panDist > kMinimumFadeGap) {
        const double dx = static_cast<double>(emitterPosition_.x) - static_cast<double>(playerPosition_.x);
        pan = std::clamp(dx / static_cast<double>(panDist), -1.0, 1.0);

        // Equal-power stereo pan curve for smoother perceived loudness.
        const double angle = (pan + 1.0) * (std::acos(-1.0) / 4.0);
        leftVolume = static_cast<int>(std::lround(static_cast<double>(volume) * std::cos(angle)));
        rightVolume = static_cast<int>(std::lround(static_cast<double>(volume) * std::sin(angle)));
    }

    if (volume == lastVolume_ &&
        leftVolume == lastLeftVolume_ &&
        rightVolume == lastRightVolume_) {
        return;
    }

    bool ok = true;
    if (backend_ == PlaybackBackend::MediaFoundationStream) {
        if (!mfState_ || !mfState_->player) {
            logger_.warn("Media Foundation player missing while applying volume.");
            return;
        }

        const float streamVolume = static_cast<float>(std::clamp(factor * gain, 0.0, 1.0));
        const HRESULT hr = mfState_->player->SetVolume(streamVolume);
        if (FAILED(hr)) {
            logger_.warn("Media Foundation SetVolume failed: " + formatHresult(hr));
            return;
        }

        leftVolume = volume;
        rightVolume = volume;
    } else if (backend_ == PlaybackBackend::DirectShowStream) {
        if (!dsState_ || !dsState_->audio) {
            leftVolume = volume;
            rightVolume = volume;
        } else {
            long dsVolume = -10000;
            const double scalar = std::clamp(factor * gain, 0.0, 1.0);
            if (scalar > 0.0001) {
                dsVolume = static_cast<long>(std::lround(2000.0 * std::log10(scalar)));
                dsVolume = std::clamp(dsVolume, static_cast<long>(-10000), static_cast<long>(0));
            }

            const HRESULT hr = dsState_->audio->put_Volume(dsVolume);
            if (FAILED(hr)) {
                logger_.warn("DirectShow put_Volume failed: " + formatHresult(hr));
                return;
            }
            leftVolume = volume;
            rightVolume = volume;
        }
    } else if (config_.enableSpatialPan && panControlsAvailable_) {
        const bool leftOk =
            mciCommandLocked(L"setaudio " + std::wstring(kAlias) + L" left volume to " + std::to_wstring(leftVolume));
        const bool rightOk =
            mciCommandLocked(L"setaudio " + std::wstring(kAlias) + L" right volume to " + std::to_wstring(rightVolume));
        if (!leftOk || !rightOk) {
            panControlsAvailable_ = false;
            if (!panUnavailableLogged_) {
                panUnavailableLogged_ = true;
                logger_.warn("Stereo pan controls unavailable on this playback device. Falling back to scalar volume fade.");
            }
            ok = mciCommandLocked(L"setaudio " + std::wstring(kAlias) + L" volume to " + std::to_wstring(volume));
            leftVolume = volume;
            rightVolume = volume;
        }
    } else {
        ok = mciCommandLocked(L"setaudio " + std::wstring(kAlias) + L" volume to " + std::to_wstring(volume));
        leftVolume = volume;
        rightVolume = volume;
    }

    if (!ok) {
        return;
    }

    lastVolume_ = volume;
    lastLeftVolume_ = leftVolume;
    lastRightVolume_ = rightVolume;

    if (config_.logFadeChanges) {
        logger_.info(
            "Fade update: distance=" + std::to_string(distance) +
            " baseVol=" + std::to_string(volume) +
            " leftVol=" + std::to_string(leftVolume) +
            " rightVol=" + std::to_string(rightVolume) +
            " pan=" + std::to_string(pan) +
            " gain=" + std::to_string(device.volumeGain));
    }
}

double RadioEngine::distanceLocked() const
{
    const double dx = static_cast<double>(playerPosition_.x) - static_cast<double>(emitterPosition_.x);
    const double dy = static_cast<double>(playerPosition_.y) - static_cast<double>(emitterPosition_.y);
    const double dz = static_cast<double>(playerPosition_.z) - static_cast<double>(emitterPosition_.z);
    return std::sqrt((dx * dx) + (dy * dy) + (dz * dz));
}

bool RadioEngine::mciCommandLocked(const std::wstring& command, std::wstring* output)
{
    std::array<wchar_t, 512> buffer{};
    MCIERROR err = mciSendStringW(
        command.c_str(),
        output != nullptr ? buffer.data() : nullptr,
        output != nullptr ? static_cast<UINT>(buffer.size()) : 0U,
        nullptr);

    if (err != 0) {
        std::array<wchar_t, 256> errText{};
        mciGetErrorStringW(err, errText.data(), static_cast<UINT>(errText.size()));
        logger_.warn("MCI command failed: " + wideToUtf8(command) + " | " + wideToUtf8(errText.data()));
        return false;
    }

    if (output != nullptr) {
        *output = buffer.data();
    }
    return true;
}

bool RadioEngine::mciStatusNumberLocked(const std::wstring& statusName, int& outValue)
{
    std::wstring out;
    if (!mciCommandLocked(L"status " + std::wstring(kAlias) + L" " + statusName, &out)) {
        return false;
    }

    try {
        outValue = std::stoi(out);
        return true;
    } catch (...) {
        return false;
    }
}

bool RadioEngine::mciStatusModeLocked(std::wstring& outMode)
{
    return mciCommandLocked(L"status " + std::wstring(kAlias) + L" mode", &outMode);
}

bool RadioEngine::mciStatusModeSilentLocked(std::wstring& outMode)
{
    std::array<wchar_t, 128> buffer{};
    const MCIERROR err =
        mciSendStringW((L"status " + std::wstring(kAlias) + L" mode").c_str(), buffer.data(), static_cast<UINT>(buffer.size()), nullptr);
    if (err != 0) {
        outMode.clear();
        return false;
    }

    outMode = buffer.data();
    return true;
}

bool RadioEngine::waitForAliasClosedLocked(std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        std::wstring mode;
        if (!mciStatusModeSilentLocked(mode)) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    std::wstring mode;
    return !mciStatusModeSilentLocked(mode);
}

void RadioEngine::cleanupCurrentStreamTempFileLocked()
{
    if (streamWrapperTempPath_.empty()) {
        return;
    }

    std::error_code ec;
    const bool removed = std::filesystem::remove(streamWrapperTempPath_, ec);
    if (!removed && ec) {
        logger_.warn("Failed to remove temp stream wrapper file: " + pathToUtf8(streamWrapperTempPath_));
    }

    streamWrapperTempPath_.clear();
}

RadioEngine::DeviceState RadioEngine::makeCurrentDeviceStateLocked() const
{
    DeviceState snapshot;
    const auto it = deviceStates_.find(currentDeviceId_);
    if (it != deviceStates_.end()) {
        snapshot.fadeOverride = it->second.fadeOverride;
        snapshot.volumeGain = it->second.volumeGain;
    }

    snapshot.selectedKey = selectedKey_;
    snapshot.mode = mode_;
    snapshot.state = state_;
    snapshot.currentTrackPath = currentTrackPath_;
    snapshot.songIndex = songIndex_;
    snapshot.transitionIndex = transitionIndex_;
    snapshot.adIndex = adIndex_;
    snapshot.songsSinceAd = songsSinceAd_;
    snapshot.previousWasSong = previousWasSong_;
    snapshot.emitterPosition = emitterPosition_;
    snapshot.playerPosition = playerPosition_;
    snapshot.lastVolume = lastVolume_;
    snapshot.lastLeftVolume = lastLeftVolume_;
    snapshot.lastRightVolume = lastRightVolume_;
    snapshot.panControlsAvailable = panControlsAvailable_;
    snapshot.panUnavailableLogged = panUnavailableLogged_;
    snapshot.trackStartTime = trackStartTime_;
    snapshot.trackStartValid = trackStartValid_;
    return snapshot;
}

void RadioEngine::applyDeviceStateLocked(const DeviceState& state)
{
    selectedKey_ = state.selectedKey;
    mode_ = state.mode;
    state_ = state.state;
    currentTrackPath_ = state.currentTrackPath;
    songIndex_ = state.songIndex;
    transitionIndex_ = state.transitionIndex;
    adIndex_ = state.adIndex;
    songsSinceAd_ = state.songsSinceAd;
    previousWasSong_ = state.previousWasSong;
    emitterPosition_ = state.emitterPosition;
    playerPosition_ = state.playerPosition;
    lastVolume_ = state.lastVolume;
    lastLeftVolume_ = state.lastLeftVolume;
    lastRightVolume_ = state.lastRightVolume;
    panControlsAvailable_ = state.panControlsAvailable;
    panUnavailableLogged_ = state.panUnavailableLogged;
    trackStartTime_ = state.trackStartTime;
    trackStartValid_ = state.trackStartValid;
}

void RadioEngine::syncCurrentDeviceStateLocked()
{
    deviceStates_[currentDeviceId_] = makeCurrentDeviceStateLocked();
}

RadioEngine::DeviceState& RadioEngine::ensureDeviceStateLocked(std::uint64_t deviceId)
{
    auto it = deviceStates_.find(deviceId);
    if (it != deviceStates_.end()) {
        return it->second;
    }

    DeviceState initial;
    initial.fadeOverride.enabled = false;
    initial.volumeGain = kDefaultVolumePercent / 100.0F;
    auto [insertIt, inserted] = deviceStates_.emplace(deviceId, std::move(initial));
    (void)inserted;
    return insertIt->second;
}

void RadioEngine::switchToDeviceLocked(std::uint64_t deviceId)
{
    if (deviceId == currentDeviceId_) {
        ensureDeviceStateLocked(deviceId);
        return;
    }

    syncCurrentDeviceStateLocked();

    if (state_ == PlaybackState::Playing || state_ == PlaybackState::Paused) {
        stopPlaybackDeviceLocked(true);
        state_ = PlaybackState::Stopped;
        trackStartValid_ = false;
        syncCurrentDeviceStateLocked();
    }

    currentDeviceId_ = deviceId;
    DeviceState& target = ensureDeviceStateLocked(deviceId);
    applyDeviceStateLocked(target);

    // Audio device is global; after switching refs we keep session state but require explicit play/start.
    if (state_ != PlaybackState::Stopped) {
        state_ = PlaybackState::Stopped;
        trackStartValid_ = false;
    }
}

bool RadioEngine::runAsyncCommandForDevice(
    std::uint64_t deviceId,
    const std::function<bool()>& command,
    const std::function<void(bool result)>& completion)
{
    std::unique_lock<std::mutex> lock(mutex_);

    if (!workerRunning_ || std::this_thread::get_id() == workerThreadId_) {
        switchToDeviceLocked(deviceId);
        bool result = false;
        try {
            result = command();
        } catch (const std::exception& ex) {
            result = false;
            logger_.error(std::string("Unhandled exception in async command: ") + ex.what());
        } catch (...) {
            result = false;
            logger_.error("Unhandled unknown exception in async command.");
        }
        syncCurrentDeviceStateLocked();
        if (completion) {
            completion(result);
        }
        return true;
    }

    commandQueue_.emplace_back([this, command, completion, deviceId]() {
        bool result = false;
        try {
            switchToDeviceLocked(deviceId);
            result = command();
        } catch (const std::exception& ex) {
            result = false;
            logger_.error(std::string("Unhandled exception in queued async command: ") + ex.what());
        } catch (...) {
            result = false;
            logger_.error("Unhandled unknown exception in queued async command.");
        }
        syncCurrentDeviceStateLocked();
        if (completion) {
            completion(result);
        }
        cv_.notify_all();
    });

    cv_.notify_all();
    return true;
}

bool RadioEngine::runBoolCommandForDevice(std::uint64_t deviceId, const std::function<bool()>& command)
{
    std::unique_lock<std::mutex> lock(mutex_);

    if (!workerRunning_) {
        switchToDeviceLocked(deviceId);
        clearPlayInterruptRequest();
        const bool result = command();
        syncCurrentDeviceStateLocked();
        return result;
    }

    if (std::this_thread::get_id() == workerThreadId_) {
        switchToDeviceLocked(deviceId);
        clearPlayInterruptRequest();
        const bool result = command();
        syncCurrentDeviceStateLocked();
        return result;
    }

    struct PendingResult
    {
        bool done{ false };
        bool result{ false };
    };

    auto pending = std::make_shared<PendingResult>();
    commandQueue_.emplace_back([this, command, pending, deviceId]() {
        try {
            switchToDeviceLocked(deviceId);
            clearPlayInterruptRequest();
            pending->result = command();
        } catch (const std::exception& ex) {
            pending->result = false;
            logger_.error(std::string("Unhandled exception in queued command: ") + ex.what());
        } catch (...) {
            pending->result = false;
            logger_.error("Unhandled unknown exception in queued command.");
        }
        syncCurrentDeviceStateLocked();
        pending->done = true;
        cv_.notify_all();
    });

    cv_.notify_all();
    const bool completed = cv_.wait_for(lock, kCommandWaitTimeout, [this, pending]() {
        return pending->done || !workerRunning_;
    });
    if (!completed) {
        logger_.error("Radio command timed out waiting for worker completion (deviceId=" +
                      std::to_string(deviceId) + ").");
        return false;
    }

    return pending->done ? pending->result : false;
}

void RadioEngine::workerLoop()
{
    logger_.info("Worker loop entered.");

    std::unique_lock<std::mutex> lock(mutex_);
    workerThreadId_ = std::this_thread::get_id();
    while (!stopWorker_) {
        cv_.wait_for(lock, std::chrono::milliseconds(100), [this]() {
            return stopWorker_ || !commandQueue_.empty();
        });
        if (stopWorker_) {
            break;
        }

        while (!commandQueue_.empty()) {
            auto command = std::move(commandQueue_.front());
            commandQueue_.pop_front();
            command();
            syncCurrentDeviceStateLocked();
            if (stopWorker_) {
                break;
            }
        }
        if (stopWorker_) {
            break;
        }

        if (state_ == PlaybackState::Playing) {
            if (isTrackCompleteLocked()) {
                (void)updateTrackLocked(true);
            } else {
                updateFadeVolumeLocked();
            }
            syncCurrentDeviceStateLocked();
        }
    }

    stopPlaybackDeviceLocked(true);
    stopFxLocked();
    state_ = PlaybackState::Stopped;
    mode_ = PlaybackMode::None;
    trackStartValid_ = false;
    shutdownDirectShowLocked();
    shutdownMediaFoundationLocked();
    syncCurrentDeviceStateLocked();
    workerThreadId_ = {};
}
