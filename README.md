# RadioSFSE (Milestone Build)

`RadioSFSE` is a Starfield SFSE plugin scaffold for an in-game MP3 radio/player system.

This milestone implements:
- Background worker thread (no periodic external tick required)
- Directory scanner for playlists and stations
- Track sequencing with station transitions + ad insertion
- Runtime control API: `change_playlist`, `play`, `start`, `pause`, `stop`, `forward`, `rewind`, `isPlaying`
- Distance-based fade plus stereo spatial pan (3D-like using emitter/player coordinates)
- Text milestone logging
- CommonLibSF-based Papyrus native registration on VM availability
- Config-driven internet stream stations (URL-based, no simulated transitions/ads)

This milestone includes Papyrus native binding through `RE::BSScript::IVirtualMachine::BindNativeMethod` and no longer depends on manual callsite/vtable hook probing.

## Directory Model

Default radio root:
- `%USERPROFILE%\\OneDrive\\Documentos\\My Games\\Starfield\\Data\\Radio`

Each subdirectory under root is treated as one channel:
- `playlist`: contains only normal tracks
- `station`: contains normal tracks + special prefixed files
- `stream station`: configured in INI with URL, acts like station but plays live stream directly

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
- `enable_spatial_pan`
- `pan_distance`
- `log_fade_changes`
- `auto_rescan_on_change_playlist`
- `loop_playlist`
- `stream_station` (repeatable: `Name|Url`)

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
- `[M4]` SFSE messaging listener install
- `[M5]` SFSE lifecycle message + VM availability tracking
- `[M6]` Papyrus natives registered via CommonLibSF

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
- `is_playing()`

`set_positions` drives distance attenuation and stereo pan updates from activator/player coordinates.

## Papyrus Test Scripts

Sample scripts in `scripts/` for quick CK wiring:
- `scripts/RadioSFSE_AutostartQuestScript.psc`
- `scripts/RadioSFSE_PlaySlateScript.psc`
- `scripts/RadioSFSE_StopSlateScript.psc`

Suggested quick test:
1. Create an auto-start quest and attach `RadioSFSE_AutostartQuestScript`.
2. Set `StartupPlaylist` to `Playlist_Default` and (optionally) assign `RadioEmitter`.
3. Create two activator/slate objects with play/stop scripts for manual control.

## Next Integration Step

1. Launch via `sfse_loader.exe` and confirm `[M6]` appears in `RadioSFSE.log`.
2. Attach a test Papyrus script that calls `RadioSFSENative` functions.
3. Verify playback controls and fade updates from `set_positions`.
