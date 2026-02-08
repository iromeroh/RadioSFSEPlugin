#pragma once

#include "logger.h"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <utility>
#include <string>
#include <thread>
#include <vector>

class RadioEngine
{
public:
    explicit RadioEngine(Logger& logger);
    ~RadioEngine();

    bool initialize();
    void shutdown();

    bool changePlaylist(const std::string& channelName);
    bool play();
    bool start();
    bool pause();
    bool stop();
    bool forward();
    bool rewind();
    bool rescanLibrary();
    bool isPlaying() const;

    bool setPositions(float emitterX, float emitterY, float emitterZ, float playerX, float playerY, float playerZ);

    std::string currentChannel() const;
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
    bool runBoolCommand(const std::function<bool()>& command);

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
