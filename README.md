# RadioSFSE (Milestone Build)

`RadioSFSE` is a Starfield SFSE plugin scaffold for an in-game MP3 radio/player system.

This milestone implements:
- Background worker thread (no periodic external tick required)
- Directory scanner for structured playlist/station folders
- Track sequencing with station transitions + ad insertion
- Runtime control API: `change_playlist`, `play`, `start`, `pause`, `stop`, `forward`, `rewind`, `isPlaying`, `changeToNextSource`, `currentSourceName`, `currentTrackBasename`, `setFadeParams`, `volumeUp`, `volumeDown`, `getVolumeStepPercent`, `getDebugVerbosity`
- Availability probe API: `pluginAvailable` (Papyrus compatibility check)
- Distance-based fade plus stereo spatial pan (3D-like using emitter/player coordinates)
- Text milestone logging
- CommonLibSF-based Papyrus native registration on VM availability
- Config-driven internet stream stations (direct stream URLs, no simulated transitions/ads)

This milestone includes Papyrus native binding through `RE::BSScript::IVirtualMachine::BindNativeMethod` and no longer depends on manual callsite/vtable hook probing.
Playback/session state is tracked per activator reference for the current game session (not persisted across saves).

## Distribution Fallback (No SFSE DLL)

`STAR_Start_Quest_Script` now includes a Papyrus-only fallback path:
- Detects native availability through `RadioSFSENative.pluginAvailable(ref)`.
- If SFSE is not available:
  - media type `2` (stations) plays CK-declared `WwiseEvent` tracks.
  - media types `1` (local files) and `3` (streams) report unsupported and play static FX.

Configure fallback station content in CK on quest script properties:
- `StationAkilaSongs`, `StationAkilaJingles`, `StationAkilaAds`
- `StationNeonSongs`, `StationNeonJingles`, `StationNeonAds`
- `StationAtlantisSongs`, `StationAtlantisJingles`, `StationAtlantisAds`
- `StationHopetownSongs`, `StationHopetownJingles`, `StationHopetownAds`
- `StationParadisoSongs`, `StationParadisoJingles`, `StationParadisoAds`

Optional fallback FX properties:
- `FallbackStaticEvent`
- `FallbackTuningShortEvent`
- `FallbackTuningLongEvent`
- `FallbackNotificationEvent`
- `FallbackNoStationEvent`

## User Track Converter

For distribution builds with fixed CK slot names, use:

- `tools/prepare_fallback_tracks.py`

This script ingests user tracks from station folders and normalizes them to deterministic
WAV slot filenames (`song_01.wav`, `jingle_01.wav`, `ad_01.wav`) under
`Data/Sound/fx/STAR_Radio/Fallback/...`.

See:

- `docs/DISTRIBUTION_AUDIO_PIPELINE.md`

Category values for `changeToNextSource`:
- `1` = playlists (`Playlists/<Name>`)
- `2` = stations (`Stations/<Name>`)
- `3` = stream stations (INI `stream_station` declaration order)

Per-device control notes:
- `setFadeParams(ref, min, max, pan)` overrides fade distances for that specific device/ref.
- Pass any negative parameter to `setFadeParams` to reset that device to global INI defaults.
- `volumeUp(ref, step)` / `volumeDown(ref, step)` adjust per-device gain for current session.
- `getVolumeStepPercent(ref)` returns INI key `volume_step_percent` for Papyrus menu logic.
- `getDebugVerbosity(ref)` returns INI key `debug_verbosity` so Papyrus trace logging can follow the same knob.

## Directory Model

Default radio root:
- `%USERPROFILE%\\OneDrive\\Documentos\\My Games\\Starfield\\Data\\Radio`

Each subdirectory under root is treated as one channel:
- `Playlists/<Name>`: one playlist source per directory
- `Stations/<Name>`: one station source per directory
- `stream station`: configured in INI with URL, acts like station but plays live stream directly

Default prefixes:
- transition: `transition_`
- ad: `ad_`

Example:
```text
Radio/
  Playlists/
    ChillPlaylist/
      track01.mp3
      track02.mp3
  Stations/
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
- `volume_step_percent`
- `debug_verbosity` (`0` quiet, `1` info+Papyrus trace, `2` extra diagnostics; overrides `log_level` and verbose diagnostic flags when set)
- `min_fade_distance`
- `max_fade_distance`
- `enable_spatial_pan`
- `pan_distance`
- `log_fade_changes`
- `auto_rescan_on_change_playlist`
- `loop_playlist`
- `stream_station` (repeatable: `Name|Url`)
  - Url should be a direct media/stream URL (for example mp3/ogg stream endpoints)

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

### Build From WSL (tested)

Known-good invocation from WSL is to run the batch file through `cmd.exe`
from inside the plugin directory:

```bash
cd "/mnt/c/Program Files (x86)/Steam/steamapps/common/Starfield/RadioSFSEPlugin"
cmd.exe /c build_windows.cmd
```

Notes:
- Prefer the command above instead of passing a fully quoted Windows path to `cmd.exe /c`.
- The full-path form is easy to over-escape in Bash and can fail with `"not recognized as an internal or external command"`.
- Successful output produces `build-vs/Release/RadioSFSE.dll`.

Copy output DLL:
- `build-vs/Release/RadioSFSE.dll` (when using `build_windows.cmd`)
- `build/Release/RadioSFSE.dll` (if you used the manual `-B build` example)
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
- `change_to_next_source(int)`
- `set_fade_params(float,float,float)`
- `volume_up(float)`
- `volume_down(float)`
- `current_source_name()`
- `current_track_basename()`

`set_positions` drives distance attenuation and stereo pan updates from activator/player coordinates.

## Papyrus Scripts

`scripts/` is the repository source of truth for Papyrus sources used by this plugin/mod setup.

Included:
- `scripts/RadioSFSENative.psc`
- `scripts/RadioSFSE_AutostartQuestScript.psc`
- `scripts/RadioSFSE_PlaySlateScript.psc`
- `scripts/RadioSFSE_StopSlateScript.psc`
- `scripts/STAR_Start_Quest_Script.psc`
- `scripts/STAR_radio_script.psc`
- `scripts/STAR_Play_Stop_Control_Slate_Script.psc`
- `scripts/STAR_Playlist_Control_Slate_Script.psc`
- `scripts/STAR_Media_Type_Control_Slate.psc`
- `scripts/STAR_Forward_Control_Slate_Script.psc`
- `scripts/STAR_Player_Alias_Script.psc`
- `scripts/STAR_Radio_Message_Script.psc`
- `scripts/STAR_Radio_Terminal_Menu_Script.psc`

## Outpost Terminal Integration

`STAR_Radio_Terminal_Menu_Script.psc` is intended for buildable outpost terminals that act as fixed radio emitters.

- Attach the script to the relevant terminal `TerminalMenu` form(s).
- Set `myQuest` to `STAR_Start_Quest`.
- Set `RadioTerminalMenu_Main` to the main terminal menu handling radio actions.
- Optionally set `RadioTerminalMenu_Submenu` when actions are in a separate submenu.
- Default menu item IDs are aligned with the portable radio controls:
  - `0` media type
  - `1` next source
  - `2` play/pause
  - `3` forward
  - `4` rewind
  - `5` volume up
  - `6` volume down

Behavior highlights:
- Entering/using the terminal promotes that terminal reference as the active radio emitter.
- Optional stop of previous emitter prevents overlapping playback when switching from another device.
- Worldspace/cell reachability behavior remains enforced by `STAR_Start_Quest_Script` timer updates.
