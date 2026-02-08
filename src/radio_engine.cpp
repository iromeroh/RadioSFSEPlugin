#include "radio_engine.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cctype>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>

#include <windows.h>
#include <mmsystem.h>

namespace
{
constexpr wchar_t kAlias[] = L"RadioSFSE";
constexpr float kMinimumFadeGap = 1.0F;

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
        return;
    }

    (void)runBoolCommand([this]() {
        stopPlaybackDeviceLocked(true);
        state_ = PlaybackState::Stopped;
        mode_ = PlaybackMode::None;
        trackStartValid_ = false;
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

bool RadioEngine::changePlaylist(const std::string& channelName)
{
    return runBoolCommand([this, channelName]() {
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
        logger_.info("change_playlist selected: " + channel->displayName +
                     " (" + (channel->type == ChannelType::Station ? "station" : "playlist") + ")");
        return true;
    });
}

bool RadioEngine::play()
{
    return runBoolCommand([this]() {
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

bool RadioEngine::start()
{
    return runBoolCommand([this]() {
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

bool RadioEngine::pause()
{
    return runBoolCommand([this]() {
        return pauseLocked();
    });
}

bool RadioEngine::stop()
{
    return runBoolCommand([this]() {
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

bool RadioEngine::forward()
{
    return runBoolCommand([this]() {
        return forwardLocked();
    });
}

bool RadioEngine::rewind()
{
    return runBoolCommand([this]() {
        return rewindLocked();
    });
}

bool RadioEngine::rescanLibrary()
{
    return runBoolCommand([this]() {
        const bool ok = scanLibraryLocked();
        if (ok) {
            logger_.info("Library rescan complete. Channels: " + std::to_string(channels_.size()));
        } else {
            logger_.warn("Library rescan failed.");
        }
        return ok;
    });
}

bool RadioEngine::isPlaying() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return state_ == PlaybackState::Playing;
}

bool RadioEngine::setPositions(float emitterX, float emitterY, float emitterZ, float playerX, float playerY, float playerZ)
{
    return runBoolCommand([this, emitterX, emitterY, emitterZ, playerX, playerY, playerZ]() {
        emitterPosition_ = Position{ emitterX, emitterY, emitterZ };
        playerPosition_ = Position{ playerX, playerY, playerZ };
        updateFadeVolumeLocked();
        return true;
    });
}

std::string RadioEngine::currentChannel() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return selectedKey_;
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
        logger_.warn("Config not found at " + path.string() + ". Using defaults.");
        return false;
    }

    std::ifstream in(path);
    if (!in.is_open()) {
        logger_.warn("Could not open config file: " + path.string());
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

    logger_.info("Config loaded. root_path=" + config_.radioRootPath.string() +
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
    const std::string ext = toLower(path.extension().string());
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

    const std::string transitionPrefixLower = toLower(config_.transitionPrefix);
    const std::string adPrefixLower = toLower(config_.adPrefix);

    if (config_.radioRootPath.empty() || !std::filesystem::exists(config_.radioRootPath)) {
        logger_.warn("Radio root path does not exist: " + config_.radioRootPath.string());
    } else {
        std::error_code ec;
        for (std::filesystem::recursive_directory_iterator it(config_.radioRootPath, ec), end; it != end && !ec; it.increment(ec)) {
            if (!it->is_directory()) {
                continue;
            }

            const auto dirPath = it->path();
            std::vector<std::filesystem::path> songs;
            std::vector<std::filesystem::path> transitions;
            std::vector<std::filesystem::path> ads;

            for (std::filesystem::directory_iterator fileIt(dirPath, ec), fileEnd; fileIt != fileEnd && !ec; fileIt.increment(ec)) {
                if (!fileIt->is_regular_file()) {
                    continue;
                }
                const auto filePath = fileIt->path();
                if (!hasAudioExtension(filePath)) {
                    continue;
                }

                const std::string stemLower = toLower(filePath.stem().string());
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

            std::filesystem::path rel = std::filesystem::relative(dirPath, config_.radioRootPath, ec);
            if (ec) {
                rel = dirPath.filename();
                ec.clear();
            }

            std::string key = toLower(rel.generic_string());
            if (key.empty()) {
                key = toLower(dirPath.filename().string());
            }

            ChannelEntry entry;
            entry.key = key;
            entry.displayName = rel.generic_string();
            entry.directoryPath = dirPath;
            entry.type = (!transitions.empty() || !ads.empty()) ? ChannelType::Station : ChannelType::Playlist;
            entry.songs = std::move(songs);
            entry.transitions = std::move(transitions);
            entry.ads = std::move(ads);

            channels_[key] = std::move(entry);
        }
    }

    addConfiguredStreamsLocked();
    return !channels_.empty();
}

void RadioEngine::addConfiguredStreamsLocked()
{
    for (const auto& [name, url] : config_.streamStations) {
        const std::string key = toLower(trim(name));
        if (key.empty() || url.empty()) {
            continue;
        }

        ChannelEntry entry;
        entry.key = key;
        entry.displayName = name;
        entry.type = ChannelType::Station;
        entry.isStream = true;
        entry.streamUrl = url;

        channels_[key] = std::move(entry);
    }
}

std::optional<RadioEngine::ChannelEntry> RadioEngine::lookupChannelLocked(const std::string& channelName) const
{
    const std::string key = toLower(trim(channelName));
    if (key.empty()) {
        return std::nullopt;
    }

    auto it = channels_.find(key);
    if (it != channels_.end()) {
        return it->second;
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

    const std::wstring openCmd = L"open " + quoteForMCI(filePath) + L" type mpegvideo alias " + kAlias;
    if (!mciCommandLocked(openCmd)) {
        state_ = PlaybackState::Stopped;
        currentTrackPath_.clear();
        lastVolume_ = -1;
        lastLeftVolume_ = -1;
        lastRightVolume_ = -1;
        trackStartValid_ = false;
        return false;
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
    state_ = PlaybackState::Playing;
    trackStartTime_ = std::chrono::steady_clock::now();
    trackStartValid_ = true;
    updateFadeVolumeLocked();

    logger_.info("Now playing: " + filePath.string());
    return true;
}

bool RadioEngine::playStreamLocked(const std::string& streamUrl)
{
    stopPlaybackDeviceLocked(true);
    if (!waitForAliasClosedLocked(std::chrono::milliseconds(150))) {
        logger_.warn("MCI alias still open before stream play. Attempting reopen anyway.");
    }

    const std::wstring quotedUrl = quoteForMCI(utf8ToWide(streamUrl));

    if (!mciCommandLocked(L"open " + quotedUrl + L" alias " + kAlias)) {
        if (!mciCommandLocked(L"open " + quotedUrl + L" type mpegvideo alias " + kAlias)) {
            state_ = PlaybackState::Stopped;
            currentTrackPath_.clear();
            lastVolume_ = -1;
            lastLeftVolume_ = -1;
            lastRightVolume_ = -1;
            trackStartValid_ = false;
            return false;
        }
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

    currentTrackPath_.clear();
    state_ = PlaybackState::Playing;
    trackStartTime_ = std::chrono::steady_clock::now();
    trackStartValid_ = true;
    updateFadeVolumeLocked();

    logger_.info("Now streaming: " + streamUrl);
    return true;
}

void RadioEngine::stopPlaybackDeviceLocked(bool closeDevice)
{
    (void)mciSendStringW((L"stop " + std::wstring(kAlias)).c_str(), nullptr, 0, nullptr);
    if (!closeDevice) {
        return;
    }

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

bool RadioEngine::resumeLocked()
{
    if (state_ != PlaybackState::Paused) {
        return false;
    }

    if (!mciCommandLocked(L"resume " + std::wstring(kAlias))) {
        if (!mciCommandLocked(L"play " + std::wstring(kAlias))) {
            logger_.warn("resume failed.");
            return false;
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

    if (!mciCommandLocked(L"pause " + std::wstring(kAlias))) {
        logger_.warn("pause failed.");
        return false;
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
        if (elapsed < std::chrono::milliseconds(800)) {
            return false;
        }
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

    const double distance = distanceLocked();
    const float minDist = config_.minFadeDistance;
    const float maxDist = config_.maxFadeDistance;

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

    const int volume = static_cast<int>(std::lround(std::clamp(factor, 0.0, 1.0) * 1000.0));

    double pan = 0.0;
    int leftVolume = volume;
    int rightVolume = volume;
    if (config_.enableSpatialPan && config_.panDistance > kMinimumFadeGap) {
        const double dx = static_cast<double>(emitterPosition_.x) - static_cast<double>(playerPosition_.x);
        pan = std::clamp(dx / static_cast<double>(config_.panDistance), -1.0, 1.0);

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
    if (config_.enableSpatialPan && panControlsAvailable_) {
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
            " pan=" + std::to_string(pan));
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

bool RadioEngine::runBoolCommand(const std::function<bool()>& command)
{
    std::unique_lock<std::mutex> lock(mutex_);

    if (!workerRunning_) {
        return command();
    }

    if (std::this_thread::get_id() == workerThreadId_) {
        return command();
    }

    struct PendingResult
    {
        bool done{ false };
        bool result{ false };
    };

    auto pending = std::make_shared<PendingResult>();
    commandQueue_.emplace_back([this, command, pending]() {
        pending->result = command();
        pending->done = true;
        cv_.notify_all();
    });

    cv_.notify_all();
    cv_.wait(lock, [this, pending]() {
        return pending->done || !workerRunning_;
    });

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
        }
    }

    stopPlaybackDeviceLocked(true);
    state_ = PlaybackState::Stopped;
    mode_ = PlaybackMode::None;
    trackStartValid_ = false;
    workerThreadId_ = {};
}
