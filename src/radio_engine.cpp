#include "radio_engine.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cctype>
#include <fstream>
#include <limits>
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
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!workerRunning_) {
            return;
        }
        stopWorker_ = true;
        cv_.notify_all();
    }

    if (worker_.joinable()) {
        worker_.join();
    }

    std::lock_guard<std::mutex> lock(mutex_);
    stopPlaybackDeviceLocked(true);
    state_ = PlaybackState::Stopped;
    mode_ = PlaybackMode::None;
    workerRunning_ = false;
    logger_.info("Radio engine shut down.");
}

bool RadioEngine::changePlaylist(const std::string& channelName)
{
    std::lock_guard<std::mutex> lock(mutex_);
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

    stopPlaybackDeviceLocked(true);
    logger_.info("change_playlist selected: " + channel->displayName +
                 " (" + (channel->type == ChannelType::Station ? "station" : "playlist") + ")");
    return true;
}

bool RadioEngine::play()
{
    std::lock_guard<std::mutex> lock(mutex_);
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
}

bool RadioEngine::start()
{
    std::lock_guard<std::mutex> lock(mutex_);
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
}

bool RadioEngine::pause()
{
    std::lock_guard<std::mutex> lock(mutex_);
    return pauseLocked();
}

bool RadioEngine::stop()
{
    std::lock_guard<std::mutex> lock(mutex_);

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

    logger_.info("stop executed. Playback reset to beginning.");
    return true;
}

bool RadioEngine::forward()
{
    std::lock_guard<std::mutex> lock(mutex_);
    return forwardLocked();
}

bool RadioEngine::rewind()
{
    std::lock_guard<std::mutex> lock(mutex_);
    return rewindLocked();
}

bool RadioEngine::rescanLibrary()
{
    std::lock_guard<std::mutex> lock(mutex_);
    const bool ok = scanLibraryLocked();
    if (ok) {
        logger_.info("Library rescan complete. Channels: " + std::to_string(channels_.size()));
    } else {
        logger_.warn("Library rescan failed.");
    }
    return ok;
}

bool RadioEngine::setPositions(float emitterX, float emitterY, float emitterZ, float playerX, float playerY, float playerZ)
{
    std::lock_guard<std::mutex> lock(mutex_);

    emitterPosition_ = Position{ emitterX, emitterY, emitterZ };
    playerPosition_ = Position{ playerX, playerY, playerZ };
    updateFadeVolumeLocked();

    return true;
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
            } else if (key == "auto_rescan_on_change_playlist") {
                config_.autoRescanOnChangePlaylist = value == "1" || toLower(value) == "true";
            } else if (key == "loop_playlist") {
                config_.loopPlaylist = value == "1" || toLower(value) == "true";
            }
        } catch (...) {
            logger_.warn("Invalid config value for key: " + key + " (" + value + ")");
        }
    }

    if (config_.maxFadeDistance < config_.minFadeDistance + kMinimumFadeGap) {
        config_.maxFadeDistance = config_.minFadeDistance + kMinimumFadeGap;
    }

    logger_.info("Config loaded. root_path=" + config_.radioRootPath.string());
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

bool RadioEngine::scanLibraryLocked()
{
    channels_.clear();

    if (config_.radioRootPath.empty() || !std::filesystem::exists(config_.radioRootPath)) {
        logger_.warn("Radio root path does not exist: " + config_.radioRootPath.string());
        return false;
    }

    const std::string transitionPrefixLower = toLower(config_.transitionPrefix);
    const std::string adPrefixLower = toLower(config_.adPrefix);

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

    return true;
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
        logger_.warn("startCurrent failed. Channel has no songs: " + channel.displayName);
        return false;
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

    const std::wstring openCmd = L"open " + quoteForMCI(filePath) + L" type mpegvideo alias " + kAlias;
    if (!mciCommandLocked(openCmd)) {
        return false;
    }

    (void)mciCommandLocked(L"set " + std::wstring(kAlias) + L" time format milliseconds");
    if (!mciCommandLocked(L"play " + std::wstring(kAlias))) {
        return false;
    }

    currentTrackPath_ = filePath;
    state_ = PlaybackState::Playing;
    updateFadeVolumeLocked();

    logger_.info("Now playing: " + filePath.string());
    return true;
}

void RadioEngine::stopPlaybackDeviceLocked(bool closeDevice)
{
    (void)mciSendStringW((L"stop " + std::wstring(kAlias)).c_str(), nullptr, 0, nullptr);
    if (closeDevice) {
        (void)mciSendStringW((L"close " + std::wstring(kAlias)).c_str(), nullptr, 0, nullptr);
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
    if (channelIt == channels_.end() || channelIt->second.songs.empty()) {
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

    const auto channelIt = channels_.find(selectedKey_);
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

    if (!force && !isTrackCompleteLocked()) {
        return true;
    }

    const auto nextTrack = advanceAndChooseNextTrackLocked();
    if (!nextTrack.has_value()) {
        stopPlaybackDeviceLocked(true);
        state_ = PlaybackState::Stopped;
        currentTrackPath_.clear();
        logger_.info("Playback reached end of queue.");
        return false;
    }

    return playPathLocked(*nextTrack);
}

bool RadioEngine::isTrackCompleteLocked()
{
    std::wstring mode;
    if (!mciStatusModeLocked(mode)) {
        return false;
    }

    const std::string modeLower = toLower(wideToUtf8(mode));
    if (modeLower == "playing" || modeLower == "paused") {
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

    int volume = static_cast<int>(std::lround(std::clamp(factor, 0.0, 1.0) * 1000.0));
    if (volume == lastVolume_) {
        return;
    }

    const bool ok = mciCommandLocked(L"setaudio " + std::wstring(kAlias) + L" volume to " + std::to_wstring(volume));
    if (ok) {
        lastVolume_ = volume;
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

void RadioEngine::workerLoop()
{
    logger_.info("Worker loop entered.");

    std::unique_lock<std::mutex> lock(mutex_);
    while (!stopWorker_) {
        cv_.wait_for(lock, std::chrono::milliseconds(100));
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
}
