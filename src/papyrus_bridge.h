#pragma once

#include "logger.h"
#include "radio_engine.h"

#include "sfse/PluginAPI.h"

#include <cstdint>
#include <string>
#include <variant>

namespace RE
{
class TESObjectREFR;
}

class PapyrusBridge
{
public:
    PapyrusBridge(Logger& logger, RadioEngine& engine);

    bool initialize(const SFSEInterface* sfse);
    void shutdown();

    bool isInstalled() const;
    bool isRegistered() const;

private:
    static PapyrusBridge* g_instance_;
    static void onSFSEMessage(SFSEMessagingInterface::Message* message);
    static const char* messageTypeName(std::uint32_t type);

    bool installMessagingListener(const SFSEInterface* sfse);
    bool tryRegisterNatives(const char* reason);

    static void nativeChangePlaylist(std::monostate, RE::TESObjectREFR* activatorRef, std::string channelName);
    static void nativePlay(std::monostate, RE::TESObjectREFR* activatorRef);
    static void nativeStart(std::monostate, RE::TESObjectREFR* activatorRef);
    static void nativePause(std::monostate, RE::TESObjectREFR* activatorRef);
    static void nativeStop(std::monostate, RE::TESObjectREFR* activatorRef);
    static void nativeForward(std::monostate, RE::TESObjectREFR* activatorRef);
    static void nativeRewind(std::monostate, RE::TESObjectREFR* activatorRef);
    static void nativeSetPositions(
        std::monostate,
        RE::TESObjectREFR* activatorRef,
        float activatorX,
        float activatorY,
        float activatorZ,
        float playerX,
        float playerY,
        float playerZ);

    Logger& logger_;
    RadioEngine& engine_;
    PluginHandle pluginHandle_{ static_cast<PluginHandle>(kPluginHandle_Invalid) };
    bool installed_{ false };
    bool registered_{ false };
    bool waitingLogged_{ false };
};
