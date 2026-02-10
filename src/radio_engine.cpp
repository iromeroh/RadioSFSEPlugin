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

#include <windows.h>
#include <mmsystem.h>

namespace
{
constexpr wchar_t kAlias[] = L"RadioSFSE";
constexpr float kMinimumFadeGap = 1.0F;
constexpr float kDefaultVolumePercent = 100.0F;
constexpr float kMaximumVolumePercent = 200.0F;

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
        return;
    }

    (void)runBoolCommandForDevice(currentDeviceId_, [this]() {
        stopPlaybackDeviceLocked(true);
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
    return runBoolCommandForDevice(deviceId, [this]() {
        return forwardLocked();
    });
}

bool RadioEngine::rewind(std::uint64_t deviceId)
{
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
        const float delta = step > 0.0F ? step : 0.1F;
        device.volumeGain = std::clamp(device.volumeGain + delta, 0.0F, 2.0F);
        updateFadeVolumeLocked();
        logger_.info("volumeUp deviceId=" + std::to_string(currentDeviceId_) +
                     " gain=" + std::to_string(device.volumeGain));
        return true;
    });
}

bool RadioEngine::volumeDown(float step, std::uint64_t deviceId)
{
    return runBoolCommandForDevice(deviceId, [this, step]() {
        DeviceState& device = ensureDeviceStateLocked(currentDeviceId_);
        const float delta = step > 0.0F ? step : 0.1F;
        device.volumeGain = std::clamp(device.volumeGain - delta, 0.0F, 2.0F);
        updateFadeVolumeLocked();
        logger_.info("volumeDown deviceId=" + std::to_string(currentDeviceId_) +
                     " gain=" + std::to_string(device.volumeGain));
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

    logger_.info("Now playing: " + pathToUtf8(filePath));
    return true;
}

bool RadioEngine::playStreamLocked(const std::string& streamUrl)
{
    stopPlaybackDeviceLocked(true);
    if (!waitForAliasClosedLocked(std::chrono::milliseconds(150))) {
        logger_.warn("MCI alias still open before stream play. Attempting reopen anyway.");
    }

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

    const std::wstring quotedUrl = quoteForMCI(utf8ToWide(directUrl));

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

    logger_.info("Now streaming: " + directUrl);
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

bool RadioEngine::runBoolCommandForDevice(std::uint64_t deviceId, const std::function<bool()>& command)
{
    std::unique_lock<std::mutex> lock(mutex_);

    if (!workerRunning_) {
        switchToDeviceLocked(deviceId);
        const bool result = command();
        syncCurrentDeviceStateLocked();
        return result;
    }

    if (std::this_thread::get_id() == workerThreadId_) {
        switchToDeviceLocked(deviceId);
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
    state_ = PlaybackState::Stopped;
    mode_ = PlaybackMode::None;
    trackStartValid_ = false;
    syncCurrentDeviceStateLocked();
    workerThreadId_ = {};
}
