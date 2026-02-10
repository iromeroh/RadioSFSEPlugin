#pragma once

#include "logger.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <utility>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

class RadioEngine
{
public:
    explicit RadioEngine(Logger& logger);
    ~RadioEngine();

    bool initialize();
    void shutdown();

    bool changePlaylist(const std::string& channelName, std::uint64_t deviceId = 0);
    bool play(std::uint64_t deviceId = 0);
    bool start(std::uint64_t deviceId = 0);
    bool pause(std::uint64_t deviceId = 0);
    bool stop(std::uint64_t deviceId = 0);
    bool forward(std::uint64_t deviceId = 0);
    bool rewind(std::uint64_t deviceId = 0);
    bool rescanLibrary(std::uint64_t deviceId = 0);
    bool isPlaying(std::uint64_t deviceId = 0) const;
    bool changeToNextSource(int category, std::uint64_t deviceId = 0);
    bool selectNextSource(int category, std::uint64_t deviceId = 0);
    bool setFadeParams(float minDistance, float maxDistance, float panDistance, std::uint64_t deviceId = 0);
    bool volumeUp(float step, std::uint64_t deviceId = 0);
    bool volumeDown(float step, std::uint64_t deviceId = 0);
    float getVolume(std::uint64_t deviceId = 0) const;
    bool setVolume(float volume, std::uint64_t deviceId = 0);
    std::string getTrack(std::uint64_t deviceId = 0) const;
    bool setTrack(const std::string& trackBasename, std::uint64_t deviceId = 0);

    bool setPositions(float emitterX, float emitterY, float emitterZ, float playerX, float playerY, float playerZ, std::uint64_t deviceId = 0);

    std::string currentChannel(std::uint64_t deviceId = 0) const;
    std::string currentSourceName(std::uint64_t deviceId = 0) const;
    std::string currentTrackBasename(std::uint64_t deviceId = 0) const;
    std::size_t channelCount() const;

private:
    enum class ChannelType
    {
        Playlist,
        Station
    };

    enum class PlaybackMode
    {
        None,
        Playlist,
        Station
    };

    enum class PlaybackState
    {
        Stopped,
        Playing,
        Paused
    };

    struct Position
    {
        float x{ 0.0F };
        float y{ 0.0F };
        float z{ 0.0F };
    };

    struct Config
    {
        std::filesystem::path radioRootPath;
        std::string transitionPrefix{ "transition_" };
        std::string adPrefix{ "ad_" };
        std::size_t adIntervalSongs{ 3 };
        float minFadeDistance{ 150.0F };
        float maxFadeDistance{ 5000.0F };
        bool enableSpatialPan{ true };
        float panDistance{ 1200.0F };
        bool logFadeChanges{ true };
        bool autoRescanOnChangePlaylist{ true };
        bool loopPlaylist{ true };
        std::vector<std::pair<std::string, std::string>> streamStations;
    };

    struct ChannelEntry
    {
        std::string key;
        std::string displayName;
        std::filesystem::path directoryPath;
        ChannelType type{ ChannelType::Playlist };
        bool isStream{ false };
        std::string streamUrl;
        std::vector<std::filesystem::path> songs;
        std::vector<std::filesystem::path> transitions;
        std::vector<std::filesystem::path> ads;
    };

    struct DeviceFadeOverride
    {
        bool enabled{ false };
        float minDistance{ 0.0F };
        float maxDistance{ 0.0F };
        float panDistance{ 0.0F };
    };

    struct DeviceState
    {
        std::string selectedKey;
        PlaybackMode mode{ PlaybackMode::None };
        PlaybackState state{ PlaybackState::Stopped };
        std::filesystem::path currentTrackPath;

        std::size_t songIndex{ 0 };
        std::size_t transitionIndex{ 0 };
        std::size_t adIndex{ 0 };
        std::size_t songsSinceAd{ 0 };
        bool previousWasSong{ false };

        Position emitterPosition{};
        Position playerPosition{};
        int lastVolume{ -1 };
        int lastLeftVolume{ -1 };
        int lastRightVolume{ -1 };
        bool panControlsAvailable{ true };
        bool panUnavailableLogged{ false };
        std::chrono::steady_clock::time_point trackStartTime{};
        bool trackStartValid{ false };

        DeviceFadeOverride fadeOverride{};
        float volumeGain{ 1.0F };
    };

    bool loadConfig();
    static std::filesystem::path configPath();
    static std::string trim(const std::string& text);
    static std::string toLower(std::string text);
    static bool hasAudioExtension(const std::filesystem::path& path);
    static std::wstring utf8ToWide(const std::string& text);
    static std::string wideToUtf8(const std::wstring& text);
    static std::wstring quoteForMCI(const std::filesystem::path& path);
    static std::wstring quoteForMCI(const std::wstring& text);

    bool scanLibraryLocked();
    void addConfiguredStreamsLocked();
    std::optional<ChannelEntry> lookupChannelLocked(const std::string& channelName) const;
    bool startCurrentLocked(PlaybackMode mode, bool resetPosition);
    bool playPathLocked(const std::filesystem::path& filePath);
    bool playStreamLocked(const std::string& streamUrl);
    void stopPlaybackDeviceLocked(bool closeDevice);
    bool resumeLocked();
    bool pauseLocked();
    bool forwardLocked();
    bool rewindLocked();
    bool updateTrackLocked(bool force);
    bool isTrackCompleteLocked();
    std::optional<std::filesystem::path> chooseCurrentTrackLocked() const;
    std::optional<std::filesystem::path> advanceAndChooseNextTrackLocked();
    void updateFadeVolumeLocked();
    double distanceLocked() const;

    bool mciCommandLocked(const std::wstring& command, std::wstring* output = nullptr);
    bool mciStatusNumberLocked(const std::wstring& statusName, int& outValue);
    bool mciStatusModeLocked(std::wstring& outMode);
    bool mciStatusModeSilentLocked(std::wstring& outMode);
    bool waitForAliasClosedLocked(std::chrono::milliseconds timeout);
    bool runBoolCommandForDevice(std::uint64_t deviceId, const std::function<bool()>& command);
    DeviceState makeCurrentDeviceStateLocked() const;
    void applyDeviceStateLocked(const DeviceState& state);
    void syncCurrentDeviceStateLocked();
    DeviceState& ensureDeviceStateLocked(std::uint64_t deviceId);
    void switchToDeviceLocked(std::uint64_t deviceId);

    void workerLoop();

    Logger& logger_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::thread worker_;
    bool workerRunning_{ false };
    bool stopWorker_{ false };
    std::thread::id workerThreadId_{};
    std::deque<std::function<void()>> commandQueue_{};

    Config config_{};
    std::map<std::string, ChannelEntry> channels_;
    std::vector<std::string> streamOrderKeys_;
    std::unordered_map<std::uint64_t, DeviceState> deviceStates_;
    std::uint64_t currentDeviceId_{ 0 };
    std::string selectedKey_;
    PlaybackMode mode_{ PlaybackMode::None };
    PlaybackState state_{ PlaybackState::Stopped };
    std::filesystem::path currentTrackPath_;

    std::size_t songIndex_{ 0 };
    std::size_t transitionIndex_{ 0 };
    std::size_t adIndex_{ 0 };
    std::size_t songsSinceAd_{ 0 };
    bool previousWasSong_{ false };

    Position emitterPosition_{};
    Position playerPosition_{};
    int lastVolume_{ -1 };
    int lastLeftVolume_{ -1 };
    int lastRightVolume_{ -1 };
    bool panControlsAvailable_{ true };
    bool panUnavailableLogged_{ false };
    std::chrono::steady_clock::time_point trackStartTime_{};
    bool trackStartValid_{ false };
};
