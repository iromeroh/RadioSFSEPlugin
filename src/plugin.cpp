#include "logger.h"
#include "papyrus_bridge.h"
#include "radio_engine.h"

#include "sfse/PluginAPI.h"
#include "sfse_common/sfse_version.h"

#include <memory>
#include <string>

#include <windows.h>

namespace
{
Logger g_logger;
std::unique_ptr<RadioEngine> g_engine;
std::unique_ptr<PapyrusBridge> g_papyrusBridge;
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

BOOL APIENTRY DllMain(HMODULE, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_DETACH) {
        if (g_papyrusBridge) {
            g_papyrusBridge->shutdown();
            g_papyrusBridge.reset();
        }
        if (g_engine) {
            g_engine->shutdown();
            g_engine.reset();
        }
    }
    return TRUE;
}
