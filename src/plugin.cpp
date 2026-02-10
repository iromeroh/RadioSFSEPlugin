#include "logger.h"
#include "papyrus_bridge.h"
#include "radio_engine.h"

#include "sfse/PluginAPI.h"
#include "sfse_common/sfse_version.h"

#include <memory>
#include <exception>
#include <string>

#include <windows.h>

namespace
{
Logger g_logger;
std::unique_ptr<RadioEngine> g_engine;
std::unique_ptr<PapyrusBridge> g_papyrusBridge;
thread_local std::string g_sourceNameBuffer;
thread_local std::string g_trackNameBuffer;
}

extern "C"
{
__declspec(dllexport) SFSEPluginVersionData SFSEPlugin_Version = {
    SFSEPluginVersionData::kVersion,
    1,
    "RadioSFSE",
    "Ivan+Codex",
    SFSEPluginVersionData::kAddressIndependence_AddressLibraryV2,
    SFSEPluginVersionData::kStructureIndependence_NoStructs,
    { RUNTIME_VERSION_1_15_222, 0 },
    0,
    0,
    0
};
}

static bool registerPapyrusBridge(const SFSEInterface* sfse)
{
    if (!g_engine) {
        return false;
    }

    g_papyrusBridge = std::make_unique<PapyrusBridge>(g_logger, *g_engine);
    return g_papyrusBridge->initialize(sfse);
}

extern "C" __declspec(dllexport) bool SFSEPlugin_Load(const SFSEInterface* sfse)
{
    try {
        (void)sfse;

        if (!g_logger.initialize()) {
            return false;
        }

        g_logger.info("[M0] SFSEPlugin_Load entered.");

        g_engine = std::make_unique<RadioEngine>(g_logger);
        if (!g_engine->initialize()) {
            g_logger.error("Radio engine failed to initialize.");
            return false;
        }

        (void)registerPapyrusBridge(sfse);

        g_logger.info("RadioSFSE loaded.");
        return true;
    } catch (const std::exception& ex) {
        g_logger.error(std::string("Unhandled exception in SFSEPlugin_Load: ") + ex.what());
        return false;
    } catch (...) {
        g_logger.error("Unhandled unknown exception in SFSEPlugin_Load.");
        return false;
    }
}

extern "C" __declspec(dllexport) bool change_playlist(const char* channelName)
{
    if (!g_engine || channelName == nullptr) {
        return false;
    }
    return g_engine->changePlaylist(channelName);
}

extern "C" __declspec(dllexport) bool play()
{
    return g_engine ? g_engine->play() : false;
}

extern "C" __declspec(dllexport) bool start()
{
    return g_engine ? g_engine->start() : false;
}

extern "C" __declspec(dllexport) bool pause()
{
    return g_engine ? g_engine->pause() : false;
}

extern "C" __declspec(dllexport) bool stop()
{
    return g_engine ? g_engine->stop() : false;
}

extern "C" __declspec(dllexport) bool forward()
{
    return g_engine ? g_engine->forward() : false;
}

extern "C" bool radio_rewind()
{
    return g_engine ? g_engine->rewind() : false;
}

extern "C" __declspec(dllexport) bool rescan()
{
    return g_engine ? g_engine->rescanLibrary() : false;
}

extern "C" __declspec(dllexport) bool set_positions(
    float emitterX,
    float emitterY,
    float emitterZ,
    float playerX,
    float playerY,
    float playerZ)
{
    if (!g_engine) {
        return false;
    }
    return g_engine->setPositions(emitterX, emitterY, emitterZ, playerX, playerY, playerZ);
}

extern "C" __declspec(dllexport) bool is_playing()
{
    return g_engine ? g_engine->isPlaying() : false;
}

extern "C" __declspec(dllexport) bool change_to_next_source(int category)
{
    return g_engine ? g_engine->changeToNextSource(category) : false;
}

extern "C" __declspec(dllexport) bool select_next_source(int category)
{
    return g_engine ? g_engine->selectNextSource(category) : false;
}

extern "C" __declspec(dllexport) bool set_fade_params(float minDistance, float maxDistance, float panDistance)
{
    return g_engine ? g_engine->setFadeParams(minDistance, maxDistance, panDistance) : false;
}

extern "C" __declspec(dllexport) bool volume_up(float step)
{
    return g_engine ? g_engine->volumeUp(step) : false;
}

extern "C" __declspec(dllexport) bool volume_down(float step)
{
    return g_engine ? g_engine->volumeDown(step) : false;
}

extern "C" __declspec(dllexport) float get_volume()
{
    return g_engine ? g_engine->getVolume() : 100.0F;
}

extern "C" __declspec(dllexport) bool set_volume(float volume)
{
    return g_engine ? g_engine->setVolume(volume) : false;
}

extern "C" __declspec(dllexport) const char* current_source_name()
{
    if (!g_engine) {
        return "";
    }

    g_sourceNameBuffer = g_engine->currentSourceName();
    return g_sourceNameBuffer.c_str();
}

extern "C" __declspec(dllexport) const char* current_track_basename()
{
    if (!g_engine) {
        return "";
    }

    g_trackNameBuffer = g_engine->currentTrackBasename();
    return g_trackNameBuffer.c_str();
}

extern "C" __declspec(dllexport) const char* get_track()
{
    if (!g_engine) {
        return "";
    }

    g_trackNameBuffer = g_engine->getTrack();
    return g_trackNameBuffer.c_str();
}

extern "C" __declspec(dllexport) bool set_track(const char* trackBasename)
{
    if (!g_engine || trackBasename == nullptr) {
        return false;
    }

    return g_engine->setTrack(trackBasename);
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
    }

    if (reason == DLL_PROCESS_DETACH) {
        // Avoid blocking work in DllMain (loader lock). Process teardown will reclaim resources.
        (void)g_papyrusBridge.release();
        (void)g_engine.release();
    }
    return TRUE;
}
