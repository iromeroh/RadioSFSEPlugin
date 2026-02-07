# RadioSFSE (Milestone Build)

`RadioSFSE` is a Starfield SFSE plugin scaffold for an in-game MP3 radio/player system.

This milestone implements:
- Background worker thread (no periodic external tick required)
- Directory scanner for playlists and stations
- Track sequencing with station transitions + ad insertion
- Runtime control API: `change_playlist`, `play`, `start`, `pause`, `stop`, `forward`, `rewind`
- Distance-based volume fade using emitter/player coordinates
- Text milestone logging
- Experimental direct Papyrus registration hook capture (Address/RVA-configurable)

This milestone does **not** include final Papyrus native registration yet. It does include a direct hook path to capture the live VM/registry pointer, but ABI details for safe native registration are still unresolved in this local SDK.

## Directory Model

Default radio root:
- `%USERPROFILE%\\OneDrive\\Documentos\\My Games\\Starfield\\Data\\Radio`

Each subdirectory under root is treated as one channel:
- `playlist`: contains only normal tracks
- `station`: contains normal tracks + special prefixed files

Default prefixes:
- transition: `transition_`
- ad: `ad_`

Example:
```text
Radio/
  ChillPlaylist/
    track01.mp3
    track02.mp3
  NewAtlantisFM/
    song01.mp3
    song02.mp3
    transition_sweep01.mp3
    ad_shipyard01.mp3
```

## Config

Copy `RadioSFSE.ini.example` to:
- `Data/SFSE/Plugins/RadioSFSE.ini`

Supported keys:
- `root_path`
- `transition_prefix`
- `ad_prefix`
- `ad_interval_songs`
- `min_fade_distance`
- `max_fade_distance`
- `auto_rescan_on_change_playlist`
- `loop_playlist`
- `experimental_papyrus_hook`
- `papyrus_hook_mode`
- `papyrus_registration_call_rva`
- `papyrus_registration_call_address`
- `papyrus_invoke_target_rva`
- `papyrus_invoke_callsite_rva`
- `papyrus_hook_verbose`

## Logs

Primary log path:
- `%USERPROFILE%\\Documents\\My Games\\Starfield\\SFSE\\Logs\\RadioSFSE.log`

Fallback:
- `Data/SFSE/Plugins/RadioSFSE.log`

Milestone markers:
- `[M0]` plugin load
- `[M1]` engine + config
- `[M2]` library scan
- `[M3]` background worker
- `[M4]` direct Papyrus hook install
- `[M5]` Papyrus VM/registry pointer capture
- `[M6]` native registration attempt checkpoint

## Build (Visual Studio / CMake)

From a Visual Studio Developer Prompt on Windows:

```powershell
cd "C:\Program Files (x86)\Steam\steamapps\common\Starfield\RadioSFSEPlugin"
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Copy output DLL:
- `build/Release/RadioSFSE.dll`
to:
- `Data/SFSE/Plugins/RadioSFSE.dll`

## Temporary Runtime Bridge

The DLL currently exports callable C symbols:
- `change_playlist(const char*)`
- `play()`
- `start()`
- `pause()`
- `stop()`
- `forward()`
- `rewind()`
- `rescan()`
- `set_positions(float,float,float,float,float,float)`

`set_positions` is the current hook point for activator/player distance fading until Papyrus native glue is wired.

## Next Integration Step

Complete direct VM native registration (option 2):
1. Set `experimental_papyrus_hook=true` and keep `papyrus_hook_mode=invoke_callsite`.
2. Launch with SFSE and confirm `[M4]` auto-discovery logs.
3. Trigger script-native activity and confirm `[M5]` VM/registry capture.
4. Add resolved ABI addresses/signatures for VM `RegisterFunction` and native wrappers, then wire `PapyrusBridge::attemptNativeRegistration()`.

`scripts/RadioSFSENative.psc` provides the intended native API surface.
