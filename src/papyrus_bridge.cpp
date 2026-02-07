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
    vm->BindNativeMethod(kScriptName, "set_positions", &PapyrusBridge::nativeSetPositions, std::nullopt, false);

    registered_ = true;
    logger_.info(std::string("[M6] Papyrus natives registered via CommonLibSF (script=") + kScriptName +
                 ", trigger=" + (reason ? reason : "unknown") + ").");

    return true;
}

void PapyrusBridge::nativeChangePlaylist(std::monostate, RE::TESObjectREFR* activatorRef, std::string channelName)
{
    PapyrusBridge* self = g_instance_;
    if (self == nullptr) {
        return;
    }

    (void)activatorRef;

    if (!self->engine_.changePlaylist(channelName)) {
        self->logger_.warn("Papyrus change_playlist failed for channel: " + channelName);
    }
}

void PapyrusBridge::nativePlay(std::monostate, RE::TESObjectREFR* activatorRef)
{
    PapyrusBridge* self = g_instance_;
    if (self == nullptr) {
        return;
    }

    (void)activatorRef;

    if (!self->engine_.play()) {
        self->logger_.warn("Papyrus play failed.");
    }
}

void PapyrusBridge::nativeStart(std::monostate, RE::TESObjectREFR* activatorRef)
{
    PapyrusBridge* self = g_instance_;
    if (self == nullptr) {
        return;
    }

    (void)activatorRef;

    if (!self->engine_.start()) {
        self->logger_.warn("Papyrus start failed.");
    }
}

void PapyrusBridge::nativePause(std::monostate, RE::TESObjectREFR* activatorRef)
{
    PapyrusBridge* self = g_instance_;
    if (self == nullptr) {
        return;
    }

    (void)activatorRef;

    if (!self->engine_.pause()) {
        self->logger_.warn("Papyrus pause failed.");
    }
}

void PapyrusBridge::nativeStop(std::monostate, RE::TESObjectREFR* activatorRef)
{
    PapyrusBridge* self = g_instance_;
    if (self == nullptr) {
        return;
    }

    (void)activatorRef;

    if (!self->engine_.stop()) {
        self->logger_.warn("Papyrus stop failed.");
    }
}

void PapyrusBridge::nativeForward(std::monostate, RE::TESObjectREFR* activatorRef)
{
    PapyrusBridge* self = g_instance_;
    if (self == nullptr) {
        return;
    }

    (void)activatorRef;

    if (!self->engine_.forward()) {
        self->logger_.warn("Papyrus forward failed.");
    }
}

void PapyrusBridge::nativeRewind(std::monostate, RE::TESObjectREFR* activatorRef)
{
    PapyrusBridge* self = g_instance_;
    if (self == nullptr) {
        return;
    }

    (void)activatorRef;

    if (!self->engine_.rewind()) {
        self->logger_.warn("Papyrus rewind failed.");
    }
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

    (void)activatorRef;

    if (!self->engine_.setPositions(activatorX, activatorY, activatorZ, playerX, playerY, playerZ)) {
        self->logger_.warn("Papyrus set_positions failed.");
    }
}
