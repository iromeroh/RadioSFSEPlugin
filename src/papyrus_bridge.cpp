#include "SFSE/Impl/PCH.h"

#include "papyrus_bridge.h"

#include "RE/A/Array.h"
#include "RE/M/MemoryManager.h"
#include "RE/B/BSScriptUtil.h"
#include "RE/T/TESObjectREFR.h"
#include "RE/V/VirtualMachine.h"

#include <cstdint>
#include <optional>
#include <variant>

namespace
{
std::uint64_t deviceKeyFromRef(RE::TESObjectREFR* activatorRef)
{
    return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(activatorRef));
}

std::string buildFailureMessage(
    RadioEngine& engine,
    const std::uint64_t deviceId,
    const std::string& commandName,
    const std::string& detail = {})
{
    if (commandName == "play" || commandName == "start" || commandName == "forward" || commandName == "rewind") {
        const std::string track = engine.getTrack(deviceId);
        const std::string source = engine.currentSourceName(deviceId);
        if (track == "na") {
            if (source.empty()) {
                return "Stream playback failed. URL/format is not supported by Windows media backend.";
            }
            return "Stream '" + source + "' failed. URL/format is not supported by Windows media backend.";
        }
        if (!detail.empty()) {
            return detail;
        }
        return "Playback command failed.";
    }

    if (commandName == "changeToNextSource" || commandName == "selectNextSource") {
        return "No media source available for the selected category.";
    }
    if (commandName == "change_playlist") {
        return "Playlist/station not found.";
    }
    if (commandName == "setTrack") {
        return "Track not found in selected playlist/station.";
    }
    if (commandName == "setVolume") {
        return "Unable to apply volume on this radio.";
    }
    if (!detail.empty()) {
        return detail;
    }
    return "Radio command failed.";
}
}

PapyrusBridge* PapyrusBridge::g_instance_ = nullptr;

PapyrusBridge::PapyrusBridge(Logger& logger, RadioEngine& engine) :
    logger_(logger),
    engine_(engine)
{
}

bool PapyrusBridge::initialize(const SFSEInterface* sfse)
{
    if (sfse == nullptr) {
        logger_.warn("[M4] PapyrusBridge initialize failed: SFSE interface is null.");
        return false;
    }

    g_instance_ = this;

    if (!installMessagingListener(sfse)) {
        g_instance_ = nullptr;
        logger_.warn("[M4] PapyrusBridge failed to install SFSE messaging listener.");
        return false;
    }

    (void)tryRegisterNatives("plugin_load");
    return true;
}

void PapyrusBridge::shutdown()
{
    if (g_instance_ == this) {
        g_instance_ = nullptr;
    }
}

bool PapyrusBridge::isInstalled() const
{
    return installed_;
}

bool PapyrusBridge::isRegistered() const
{
    return registered_;
}

const char* PapyrusBridge::messageTypeName(const std::uint32_t type)
{
    switch (type) {
    case SFSEMessagingInterface::kMessage_PostLoad:
        return "PostLoad";
    case SFSEMessagingInterface::kMessage_PostPostLoad:
        return "PostPostLoad";
    case SFSEMessagingInterface::kMessage_PostDataLoad:
        return "PostDataLoad";
    case SFSEMessagingInterface::kMessage_PostPostDataLoad:
        return "PostPostDataLoad";
    case SFSEMessagingInterface::kMessage_PreSaveGame:
        return "PreSaveGame";
    case SFSEMessagingInterface::kMessage_PostSaveGame:
        return "PostSaveGame";
    case SFSEMessagingInterface::kMessage_PreLoadGame:
        return "PreLoadGame";
    case SFSEMessagingInterface::kMessage_PostLoadGame:
        return "PostLoadGame";
    default:
        return "Unknown";
    }
}

void PapyrusBridge::onSFSEMessage(SFSEMessagingInterface::Message* message)
{
    PapyrusBridge* self = g_instance_;
    if (self == nullptr || message == nullptr) {
        return;
    }

    self->logger_.info(std::string("[M5] SFSE message received: ") + messageTypeName(message->type));
    (void)self->tryRegisterNatives(messageTypeName(message->type));
}

bool PapyrusBridge::installMessagingListener(const SFSEInterface* sfse)
{
    auto* messaging = static_cast<SFSEMessagingInterface*>(sfse->QueryInterface(kInterface_Messaging));
    if (messaging == nullptr) {
        logger_.warn("[M4] Messaging interface unavailable.");
        return false;
    }

    pluginHandle_ = sfse->GetPluginHandle();
    if (pluginHandle_ == kPluginHandle_Invalid) {
        logger_.warn("[M4] Invalid plugin handle from SFSE.");
        return false;
    }

    if (!messaging->RegisterListener(pluginHandle_, "SFSE", &PapyrusBridge::onSFSEMessage)) {
        logger_.warn("[M4] Failed to register SFSE messaging listener.");
        return false;
    }

    installed_ = true;
    logger_.info("[M4] PapyrusBridge listener installed (CommonLibSF path).");
    return true;
}

bool PapyrusBridge::tryRegisterNatives(const char* reason)
{
    if (registered_) {
        return true;
    }

    auto* vmImpl = RE::BSScript::Internal::VirtualMachine::GetSingleton();
    if (vmImpl == nullptr) {
        if (!waitingLogged_) {
            waitingLogged_ = true;
            logger_.info(std::string("[M5] Waiting for Papyrus VM before native registration (trigger=") +
                         (reason ? reason : "unknown") + ").");
        }
        return false;
    }

    auto* vm = static_cast<RE::BSScript::IVirtualMachine*>(vmImpl);
    if (vm == nullptr) {
        logger_.warn("[M6] VM pointer cast failed.");
        return false;
    }

    waitingLogged_ = false;

    constexpr const char* kScriptName = "RadioSFSENative";

    vm->BindNativeMethod(kScriptName, "change_playlist", &PapyrusBridge::nativeChangePlaylist, std::nullopt, false);
    vm->BindNativeMethod(kScriptName, "play", &PapyrusBridge::nativePlay, std::nullopt, false);
    vm->BindNativeMethod(kScriptName, "start", &PapyrusBridge::nativeStart, std::nullopt, false);
    vm->BindNativeMethod(kScriptName, "pause", &PapyrusBridge::nativePause, std::nullopt, false);
    vm->BindNativeMethod(kScriptName, "stop", &PapyrusBridge::nativeStop, std::nullopt, false);
    vm->BindNativeMethod(kScriptName, "forward", &PapyrusBridge::nativeForward, std::nullopt, false);
    vm->BindNativeMethod(kScriptName, "rewind", &PapyrusBridge::nativeRewind, std::nullopt, false);
    vm->BindNativeMethod(kScriptName, "isPlaying", &PapyrusBridge::nativeIsPlaying, std::nullopt, false);
    vm->BindNativeMethod(kScriptName, "currentSourceName", &PapyrusBridge::nativeCurrentSourceName, std::nullopt, false);
    vm->BindNativeMethod(kScriptName, "currentTrackBasename", &PapyrusBridge::nativeCurrentTrackBasename, std::nullopt, false);
    vm->BindNativeMethod(kScriptName, "changeToNextSource", &PapyrusBridge::nativeChangeToNextSource, std::nullopt, false);
    vm->BindNativeMethod(kScriptName, "selectNextSource", &PapyrusBridge::nativeSelectNextSource, std::nullopt, false);
    vm->BindNativeMethod(kScriptName, "setFadeParams", &PapyrusBridge::nativeSetFadeParams, std::nullopt, false);
    vm->BindNativeMethod(kScriptName, "volumeUp", &PapyrusBridge::nativeVolumeUp, std::nullopt, false);
    vm->BindNativeMethod(kScriptName, "volumeDown", &PapyrusBridge::nativeVolumeDown, std::nullopt, false);
    vm->BindNativeMethod(kScriptName, "getVolume", &PapyrusBridge::nativeGetVolume, std::nullopt, false);
    vm->BindNativeMethod(kScriptName, "setVolume", &PapyrusBridge::nativeSetVolume, std::nullopt, false);
    vm->BindNativeMethod(kScriptName, "getTrack", &PapyrusBridge::nativeGetTrack, std::nullopt, false);
    vm->BindNativeMethod(kScriptName, "setTrack", &PapyrusBridge::nativeSetTrack, std::nullopt, false);
    vm->BindNativeMethod(kScriptName, "lastError", &PapyrusBridge::nativeLastError, std::nullopt, false);
    vm->BindNativeMethod(kScriptName, "set_positions", &PapyrusBridge::nativeSetPositions, std::nullopt, false);

    registered_ = true;
    logger_.info(std::string("[M6] Papyrus natives registered via CommonLibSF (script=") + kScriptName +
                 ", trigger=" + (reason ? reason : "unknown") + ").");

    return true;
}

bool PapyrusBridge::shouldAcceptCommand(const char* commandName, const void* activatorRef)
{
    const auto now = std::chrono::steady_clock::now();
    const std::uintptr_t refKey = reinterpret_cast<std::uintptr_t>(activatorRef);
    const std::string command = commandName ? commandName : "";

    std::lock_guard<std::mutex> lock(commandMutex_);
    if (command == lastCommandName_ && refKey == lastCommandRef_) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastCommandTime_);
        if (elapsed < commandDebounce_) {
            logger_.info("Papyrus duplicate command ignored: " + command +
                         " ref=0x" + std::to_string(refKey));
            return false;
        }
    }

    lastCommandName_ = command;
    lastCommandRef_ = refKey;
    lastCommandTime_ = now;
    return true;
}

void PapyrusBridge::setLastError(const std::uint64_t deviceId, const std::string& message)
{
    std::lock_guard<std::mutex> lock(lastErrorMutex_);
    if (message.empty()) {
        lastErrorByDevice_.erase(deviceId);
        return;
    }
    lastErrorByDevice_[deviceId] = message;
}

void PapyrusBridge::clearLastError(const std::uint64_t deviceId)
{
    std::lock_guard<std::mutex> lock(lastErrorMutex_);
    lastErrorByDevice_.erase(deviceId);
}

std::string PapyrusBridge::getLastError(const std::uint64_t deviceId) const
{
    std::lock_guard<std::mutex> lock(lastErrorMutex_);
    const auto it = lastErrorByDevice_.find(deviceId);
    if (it == lastErrorByDevice_.end()) {
        return {};
    }
    return it->second;
}

void PapyrusBridge::nativeChangePlaylist(std::monostate, RE::TESObjectREFR* activatorRef, std::string channelName)
{
    PapyrusBridge* self = g_instance_;
    if (self == nullptr) {
        return;
    }

    if (!self->shouldAcceptCommand("change_playlist", activatorRef)) {
        return;
    }

    const std::uint64_t deviceId = deviceKeyFromRef(activatorRef);
    if (!self->engine_.changePlaylist(channelName, deviceId)) {
        const std::string message = buildFailureMessage(self->engine_, deviceId, "change_playlist");
        self->setLastError(deviceId, message);
        self->logger_.warn("Papyrus change_playlist failed for channel: " + channelName + " | " + message);
        return;
    }
    self->clearLastError(deviceId);
}

void PapyrusBridge::nativePlay(std::monostate, RE::TESObjectREFR* activatorRef)
{
    PapyrusBridge* self = g_instance_;
    if (self == nullptr) {
        return;
    }

    if (!self->shouldAcceptCommand("play", activatorRef)) {
        return;
    }

    const std::uint64_t deviceId = deviceKeyFromRef(activatorRef);
    if (!self->engine_.play(deviceId)) {
        const std::string message = buildFailureMessage(self->engine_, deviceId, "play");
        self->setLastError(deviceId, message);
        self->logger_.warn("Papyrus play failed. " + message);
        return;
    }
    self->clearLastError(deviceId);
}

void PapyrusBridge::nativeStart(std::monostate, RE::TESObjectREFR* activatorRef)
{
    PapyrusBridge* self = g_instance_;
    if (self == nullptr) {
        return;
    }

    if (!self->shouldAcceptCommand("start", activatorRef)) {
        return;
    }

    const std::uint64_t deviceId = deviceKeyFromRef(activatorRef);
    if (!self->engine_.start(deviceId)) {
        const std::string message = buildFailureMessage(self->engine_, deviceId, "start");
        self->setLastError(deviceId, message);
        self->logger_.warn("Papyrus start failed. " + message);
        return;
    }
    self->clearLastError(deviceId);
}

void PapyrusBridge::nativePause(std::monostate, RE::TESObjectREFR* activatorRef)
{
    PapyrusBridge* self = g_instance_;
    if (self == nullptr) {
        return;
    }

    if (!self->shouldAcceptCommand("pause", activatorRef)) {
        return;
    }

    const std::uint64_t deviceId = deviceKeyFromRef(activatorRef);
    if (!self->engine_.pause(deviceId)) {
        self->setLastError(deviceId, "Unable to pause playback.");
        self->logger_.warn("Papyrus pause failed.");
        return;
    }
    self->clearLastError(deviceId);
}

void PapyrusBridge::nativeStop(std::monostate, RE::TESObjectREFR* activatorRef)
{
    PapyrusBridge* self = g_instance_;
    if (self == nullptr) {
        return;
    }

    if (!self->shouldAcceptCommand("stop", activatorRef)) {
        return;
    }

    const std::uint64_t deviceId = deviceKeyFromRef(activatorRef);
    if (!self->engine_.stop(deviceId)) {
        self->setLastError(deviceId, "Unable to stop playback.");
        self->logger_.warn("Papyrus stop failed.");
        return;
    }
    self->clearLastError(deviceId);
}

void PapyrusBridge::nativeForward(std::monostate, RE::TESObjectREFR* activatorRef)
{
    PapyrusBridge* self = g_instance_;
    if (self == nullptr) {
        return;
    }

    if (!self->shouldAcceptCommand("forward", activatorRef)) {
        return;
    }

    const std::uint64_t deviceId = deviceKeyFromRef(activatorRef);
    if (!self->engine_.forward(deviceId)) {
        const std::string message = buildFailureMessage(self->engine_, deviceId, "forward");
        self->setLastError(deviceId, message);
        self->logger_.warn("Papyrus forward failed. " + message);
        return;
    }
    self->clearLastError(deviceId);
}

void PapyrusBridge::nativeRewind(std::monostate, RE::TESObjectREFR* activatorRef)
{
    PapyrusBridge* self = g_instance_;
    if (self == nullptr) {
        return;
    }

    if (!self->shouldAcceptCommand("rewind", activatorRef)) {
        return;
    }

    const std::uint64_t deviceId = deviceKeyFromRef(activatorRef);
    if (!self->engine_.rewind(deviceId)) {
        const std::string message = buildFailureMessage(self->engine_, deviceId, "rewind");
        self->setLastError(deviceId, message);
        self->logger_.warn("Papyrus rewind failed. " + message);
        return;
    }
    self->clearLastError(deviceId);
}

bool PapyrusBridge::nativeIsPlaying(std::monostate, RE::TESObjectREFR* activatorRef)
{
    PapyrusBridge* self = g_instance_;
    if (self == nullptr) {
        return false;
    }

    return self->engine_.isPlaying(deviceKeyFromRef(activatorRef));
}

std::string PapyrusBridge::nativeCurrentSourceName(std::monostate, RE::TESObjectREFR* activatorRef)
{
    PapyrusBridge* self = g_instance_;
    if (self == nullptr) {
        return {};
    }

    return self->engine_.currentSourceName(deviceKeyFromRef(activatorRef));
}

std::string PapyrusBridge::nativeCurrentTrackBasename(std::monostate, RE::TESObjectREFR* activatorRef)
{
    PapyrusBridge* self = g_instance_;
    if (self == nullptr) {
        return {};
    }

    return self->engine_.currentTrackBasename(deviceKeyFromRef(activatorRef));
}

bool PapyrusBridge::nativeChangeToNextSource(std::monostate, RE::TESObjectREFR* activatorRef, std::int32_t category)
{
    PapyrusBridge* self = g_instance_;
    if (self == nullptr) {
        return false;
    }

    if (!self->shouldAcceptCommand("changeToNextSource", activatorRef)) {
        return false;
    }

    const std::uint64_t deviceId = deviceKeyFromRef(activatorRef);
    const bool ok = self->engine_.changeToNextSource(static_cast<int>(category), deviceId);
    if (!ok) {
        self->setLastError(deviceId, buildFailureMessage(self->engine_, deviceId, "changeToNextSource"));
    } else {
        self->clearLastError(deviceId);
    }
    return ok;
}

bool PapyrusBridge::nativeSelectNextSource(std::monostate, RE::TESObjectREFR* activatorRef, std::int32_t category)
{
    PapyrusBridge* self = g_instance_;
    if (self == nullptr) {
        return false;
    }

    if (!self->shouldAcceptCommand("selectNextSource", activatorRef)) {
        return false;
    }

    const std::uint64_t deviceId = deviceKeyFromRef(activatorRef);
    const bool ok = self->engine_.selectNextSource(static_cast<int>(category), deviceId);
    if (!ok) {
        self->setLastError(deviceId, buildFailureMessage(self->engine_, deviceId, "selectNextSource"));
    } else {
        self->clearLastError(deviceId);
    }
    return ok;
}

bool PapyrusBridge::nativeSetFadeParams(
    std::monostate,
    RE::TESObjectREFR* activatorRef,
    float minDistance,
    float maxDistance,
    float panDistance)
{
    PapyrusBridge* self = g_instance_;
    if (self == nullptr) {
        return false;
    }

    if (!self->shouldAcceptCommand("setFadeParams", activatorRef)) {
        return false;
    }

    const std::uint64_t deviceId = deviceKeyFromRef(activatorRef);
    const bool ok = self->engine_.setFadeParams(minDistance, maxDistance, panDistance, deviceId);
    if (!ok) {
        self->setLastError(deviceId, "Unable to apply fade parameters.");
    } else {
        self->clearLastError(deviceId);
    }
    return ok;
}

bool PapyrusBridge::nativeVolumeUp(std::monostate, RE::TESObjectREFR* activatorRef, float step)
{
    PapyrusBridge* self = g_instance_;
    if (self == nullptr) {
        return false;
    }

    if (!self->shouldAcceptCommand("volumeUp", activatorRef)) {
        return false;
    }

    const std::uint64_t deviceId = deviceKeyFromRef(activatorRef);
    const bool ok = self->engine_.volumeUp(step, deviceId);
    if (!ok) {
        self->setLastError(deviceId, "Unable to increase volume.");
    } else {
        self->clearLastError(deviceId);
    }
    return ok;
}

bool PapyrusBridge::nativeVolumeDown(std::monostate, RE::TESObjectREFR* activatorRef, float step)
{
    PapyrusBridge* self = g_instance_;
    if (self == nullptr) {
        return false;
    }

    if (!self->shouldAcceptCommand("volumeDown", activatorRef)) {
        return false;
    }

    const std::uint64_t deviceId = deviceKeyFromRef(activatorRef);
    const bool ok = self->engine_.volumeDown(step, deviceId);
    if (!ok) {
        self->setLastError(deviceId, "Unable to decrease volume.");
    } else {
        self->clearLastError(deviceId);
    }
    return ok;
}

float PapyrusBridge::nativeGetVolume(std::monostate, RE::TESObjectREFR* activatorRef)
{
    PapyrusBridge* self = g_instance_;
    if (self == nullptr) {
        return 100.0F;
    }

    return self->engine_.getVolume(deviceKeyFromRef(activatorRef));
}

bool PapyrusBridge::nativeSetVolume(std::monostate, RE::TESObjectREFR* activatorRef, float volume)
{
    PapyrusBridge* self = g_instance_;
    if (self == nullptr) {
        return false;
    }

    if (!self->shouldAcceptCommand("setVolume", activatorRef)) {
        return false;
    }

    const std::uint64_t deviceId = deviceKeyFromRef(activatorRef);
    const bool ok = self->engine_.setVolume(volume, deviceId);
    if (!ok) {
        self->setLastError(deviceId, buildFailureMessage(self->engine_, deviceId, "setVolume"));
    } else {
        self->clearLastError(deviceId);
    }
    return ok;
}

std::string PapyrusBridge::nativeGetTrack(std::monostate, RE::TESObjectREFR* activatorRef)
{
    PapyrusBridge* self = g_instance_;
    if (self == nullptr) {
        return {};
    }

    return self->engine_.getTrack(deviceKeyFromRef(activatorRef));
}

bool PapyrusBridge::nativeSetTrack(std::monostate, RE::TESObjectREFR* activatorRef, std::string trackBasename)
{
    PapyrusBridge* self = g_instance_;
    if (self == nullptr) {
        return false;
    }

    if (!self->shouldAcceptCommand("setTrack", activatorRef)) {
        return false;
    }

    const std::uint64_t deviceId = deviceKeyFromRef(activatorRef);
    const bool ok = self->engine_.setTrack(trackBasename, deviceId);
    if (!ok) {
        self->setLastError(deviceId, buildFailureMessage(self->engine_, deviceId, "setTrack"));
    } else {
        self->clearLastError(deviceId);
    }
    return ok;
}

std::string PapyrusBridge::nativeLastError(std::monostate, RE::TESObjectREFR* activatorRef)
{
    PapyrusBridge* self = g_instance_;
    if (self == nullptr) {
        return {};
    }

    return self->getLastError(deviceKeyFromRef(activatorRef));
}

void PapyrusBridge::nativeSetPositions(
    std::monostate,
    RE::TESObjectREFR* activatorRef,
    float activatorX,
    float activatorY,
    float activatorZ,
    float playerX,
    float playerY,
    float playerZ)
{
    PapyrusBridge* self = g_instance_;
    if (self == nullptr) {
        return;
    }

    const std::uint64_t deviceId = deviceKeyFromRef(activatorRef);
    if (!self->engine_.setPositions(
            activatorX,
            activatorY,
            activatorZ,
            playerX,
            playerY,
            playerZ,
            deviceId)) {
        self->setLastError(deviceId, "Unable to update positional audio sample.");
        self->logger_.warn("Papyrus set_positions failed.");
        return;
    }
    self->clearLastError(deviceId);
}
