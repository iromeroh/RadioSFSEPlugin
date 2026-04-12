# RadioSFSENative Modder Guide

This document explains how to call `RadioSFSENative.psc` from third-party Starfield mods.

It is aimed at mod authors who want to:

- control RadioSFSE playback from their own Papyrus scripts
- switch playlists, stations, and streams
- query playback state
- apply volume and fade controls
- drive positional audio from their own placed emitter

## What `RadioSFSENative` Is

`RadioSFSENative.psc` is the Papyrus native interface exposed by the `RadioSFSE.dll` SFSE plugin.

Source file:

```psc
Scriptname RadioSFSENative Hidden Native

; Native API surface bound by RadioSFSE.

Function change_playlist(ObjectReference activatorRef, String channelName) Global Native
Function play(ObjectReference activatorRef) Global Native
Function start(ObjectReference activatorRef) Global Native
Function pause(ObjectReference activatorRef) Global Native
Function stop(ObjectReference activatorRef) Global Native
Function forward(ObjectReference activatorRef) Global Native
Function rewind(ObjectReference activatorRef) Global Native
Function previous(ObjectReference activatorRef) Global Native
Bool Function pluginAvailable(ObjectReference activatorRef) Global Native
Bool Function isPlaying(ObjectReference activatorRef) Global Native
String Function currentSourceName(ObjectReference activatorRef) Global Native
String Function currentTrackBasename(ObjectReference activatorRef) Global Native
Bool Function changeToNextSource(ObjectReference activatorRef, Int category) Global Native
Bool Function selectNextSource(ObjectReference activatorRef, Int category) Global Native
Bool Function setFadeParams(ObjectReference activatorRef, Float minDistance, Float maxDistance, Float panDistance) Global Native
Bool Function volumeUp(ObjectReference activatorRef, Float step) Global Native
Bool Function volumeDown(ObjectReference activatorRef, Float step) Global Native
Float Function getVolume(ObjectReference activatorRef) Global Native
Int Function getMediaType(ObjectReference activatorRef) Global Native
Int Function getPlayMode(ObjectReference activatorRef) Global Native
Float Function getVolumeStepPercent(ObjectReference activatorRef) Global Native
Int Function getDebugVerbosity(ObjectReference activatorRef) Global Native
Bool Function setVolume(ObjectReference activatorRef, Float vol) Global Native
Bool Function setPlayMode(ObjectReference activatorRef, Int playMode) Global Native
String Function getTrack(ObjectReference activatorRef) Global Native
Bool Function setTrack(ObjectReference activatorRef, String trackBasename) Global Native
Bool Function playFx(ObjectReference activatorRef, String fxBasename) Global Native
Bool Function stopFx(ObjectReference activatorRef) Global Native
String Function lastError(ObjectReference activatorRef) Global Native

; Activator/player positional data feed for fade calculations.
Function set_positions(ObjectReference activatorRef, Float activatorX, Float activatorY, Float activatorZ, Float playerX, Float playerY, Float playerZ, Float playerYawDeg) Global Native
```

The native script is `Hidden Native`, so you do not attach it to an object. You call its `Global Native` functions directly from your own scripts.

## Requirements

Your users need all of the following:

- `SFSE`
- `Data/SFSE/Plugins/RadioSFSE.dll`
- `Data/SFSE/Plugins/RadioSFSE.ini`
- a populated `Data/Radio` library or configured stream stations

At compile time, your mod also needs access to `RadioSFSENative.psc` in a Papyrus source path.

## Basic Usage Pattern

1. Choose a reference to act as the radio emitter.
2. Probe native availability with `pluginAvailable(ref)`.
3. Select a source or category.
4. Start playback.
5. Poll `isPlaying(ref)` and `lastError(ref)` when you need confirmation.
6. If you use 3D-ish fade/pan behavior, keep feeding `set_positions(...)`.

Minimal example:

```psc
Scriptname MyRadioController extends Quest

ObjectReference Property MyEmitter Auto Mandatory

Bool Function EnsureRadioReady()
    if MyEmitter == None
        Debug.Trace("MyRadioController: missing emitter")
        return false
    endif

    if !RadioSFSENative.pluginAvailable(MyEmitter)
        Debug.Trace("MyRadioController: RadioSFSE not available")
        return false
    endif

    return true
EndFunction

Function StartMyStation()
    if !EnsureRadioReady()
        return
    endif

    RadioSFSENative.change_playlist(MyEmitter, "Stations/My Custom Station")
    Utility.Wait(0.25)

    RadioSFSENative.play(MyEmitter)
EndFunction
```

## Important Behavior Notes

### 1. The `activatorRef` matters

Most functions take an `ObjectReference activatorRef`.

Pass a real reference:

- a placed activator
- a terminal
- a persistent marker
- the player, if that is your intended radio device

Do not pass `None` unless you are prepared for default/fallback behavior.

### 2. Current build uses a shared device key

At the native bridge level, all non-`None` refs currently map to one shared radio device, not true isolated per-ref playback.

Practical consequence:

- use one controlling emitter per system
- do not assume two different refs can play independently at the same time
- still pass a stable ref consistently, because duplicate-command suppression is keyed by command name plus ref pointer

### 3. Some commands are asynchronous

These queue work and return immediately:

- `change_playlist`
- `play`
- `start`
- `stop`
- `forward`
- `rewind`
- `previous`
- `setTrack`
- `playFx`
- `stopFx`

For those calls:

- no immediate success means playback succeeded
- check `isPlaying(ref)`, `currentTrackBasename(ref)`, and `lastError(ref)` after a short wait

### 4. Duplicate commands are debounced

The bridge ignores the same command sent to the same ref within about `200 ms`.

If you intentionally chain commands, insert a short wait:

```psc
Utility.Wait(0.25)
```

### 5. Empty categories are valid now

`changeToNextSource(ref, category)` can now succeed even when that category has no selectable source yet.

That means:

- the media type can still change
- `currentSourceName(ref)` may remain empty
- `play(ref)` will still fail until a source exists

This is intentional and useful for fresh installs.

## Categories and Modes

Media type categories used by `changeToNextSource` and `selectNextSource`:

- `1` = local playlists from `Data/Radio/Playlists/<Name>`
- `2` = local stations from `Data/Radio/Stations/<Name>`
- `3` = stream stations from `RadioSFSE.ini`

Play mode values used by `getPlayMode` and `setPlayMode`:

- `1` = alphabetical
- `2` = shuffle

Volume values:

- `0.0` to `200.0`
- `100.0` is normal/default

## Source Selection API

### `change_playlist(ref, channelName)`

Selects a source by name/path. This is the most direct way to target a specific playlist or station.

Accepted forms are forgiving. These are the safest:

- `"Playlists/My Playlist"`
- `"Stations/My Station"`
- `"stream/my stream name"` is not recommended; prefer category-based stream cycling unless you know the exact configured name

Because this call is async, wait briefly before checking the result.

Example:

```psc
Function SelectSpecificSource(ObjectReference ref, String sourceName)
    RadioSFSENative.change_playlist(ref, sourceName)
    Utility.Wait(0.25)

    String err = RadioSFSENative.lastError(ref)
    if err != ""
        Debug.Trace("Radio select failed: " + err)
        return
    endif

    Debug.Trace("Radio source: " + RadioSFSENative.currentSourceName(ref))
EndFunction
```

### `changeToNextSource(ref, category)`

Selects the first source in a category and switches media type.

Returns:

- `true` if the category switch succeeded
- `false` only if the category is invalid or the command failed

Note:

- for an empty category, this may still return `true`
- `currentSourceName(ref)` can be empty after success

### `selectNextSource(ref, category)`

Cycles to the next source inside a category.

Use this for “next station” or “next playlist” UI buttons.

Returns:

- `true` if a next source was selected
- `false` if there are no sources in that category or selection failed

## Playback API

### `play(ref)`

Starts playback of the currently selected source, or resumes if paused.

This is the usual call for UI “Play” behavior.

### `start(ref)`

Also starts playback. In practice you can treat `play` and `start` similarly unless you are matching an existing UI flow that already uses one or the other.

### `pause(ref)`

Pauses playback immediately.

### `stop(ref)`

Stops playback asynchronously.

### `forward(ref)`, `rewind(ref)`, `previous(ref)`

Track navigation commands.

For stream stations, behavior is constrained by the stream backend. Do not assume full track-like navigation for streams.

### Playback confirmation pattern

```psc
Bool Function WaitForPlayback(ObjectReference ref, Float timeoutSeconds = 6.0)
    Float elapsed = 0.0
    while elapsed < timeoutSeconds
        if RadioSFSENative.isPlaying(ref)
            return true
        endif

        String err = RadioSFSENative.lastError(ref)
        if err != ""
            Debug.Trace("Radio playback failed: " + err)
            return false
        endif

        Utility.Wait(0.25)
        elapsed += 0.25
    endwhile

    return RadioSFSENative.isPlaying(ref)
EndFunction
```

## Query API

### `pluginAvailable(ref)`

Returns `true` when the native interface is callable.

Use this before your first real command.

### `isPlaying(ref)`

Returns current playback state.

### `currentSourceName(ref)`

Returns the display name of the selected source.

Can be empty when:

- nothing is selected
- the selected category is empty
- a command has not finished yet

### `currentTrackBasename(ref)`

Returns the current track filename only while playing or paused.

If stopped, it returns an empty string.

### `getTrack(ref)`

Returns the selected/current local track basename, even outside some UI flows.

Special case:

- for streams it returns `"na"`

### `getMediaType(ref)`

Returns the current media type:

- `1`
- `2`
- `3`

### `lastError(ref)`

Returns the last error string for that device context, or `""` if there is no current error.

This is your main diagnostic surface from Papyrus.

## Volume and Play Mode API

### `getVolume(ref)` / `setVolume(ref, volume)`

Gets or sets volume in percent.

Recommended range:

- `0.0` to `200.0`

Example:

```psc
Function SetHalfVolume(ObjectReference ref)
    if !RadioSFSENative.setVolume(ref, 50.0)
        Debug.Trace("setVolume failed: " + RadioSFSENative.lastError(ref))
    endif
EndFunction
```

### `volumeUp(ref, step)` / `volumeDown(ref, step)`

Adjust relative volume by `step` percent.

If `step <= 0`, the native layer falls back to its default step.

### `getVolumeStepPercent(ref)`

Returns the configured UI step from the INI.

Useful if you want your own menu to match the plugin's configured increment.

### `getPlayMode(ref)` / `setPlayMode(ref, playMode)`

Use:

- `1` for alphabetical
- `2` for shuffle

Any non-`2` value currently resolves to alphabetical.

## Track API

### `setTrack(ref, trackBasename)`

Queues a direct track selection inside the currently selected local playlist/station.

Returns:

- `true` if the request was queued
- `false` if queueing failed immediately

Important:

- this is not final playback success
- the source must already be selected
- the source cannot be a stream
- the track is matched by filename or stem, case-insensitive

Example:

```psc
Function PlaySpecificTrack(ObjectReference ref, String sourceName, String trackName)
    RadioSFSENative.change_playlist(ref, sourceName)
    Utility.Wait(0.25)

    if !RadioSFSENative.setTrack(ref, trackName)
        Debug.Trace("setTrack queue failed: " + RadioSFSENative.lastError(ref))
        return
    endif

    Utility.Wait(0.25)
    RadioSFSENative.play(ref)
EndFunction
```

## FX API

### `playFx(ref, fxBasename)`

Plays a one-shot FX file from `Data/Radio/FX`.

FX matching is case-insensitive and works with either:

- full filename, for example `"tuning_short.mp3"`
- stem only, for example `"tuning_short"`

Returns:

- `true` if the command was queued
- `false` if queueing failed immediately

### `stopFx(ref)`

Stops currently playing FX.

## Positional Audio API

### `setFadeParams(ref, minDistance, maxDistance, panDistance)`

Overrides fade/pan distances for the current device.

Rules:

- any negative parameter resets that ref to global INI defaults
- `maxDistance` is forced to be at least slightly larger than `minDistance`
- `panDistance` is forced to a small minimum if too small

Example:

```psc
Function UseTightInteriorFade(ObjectReference ref)
    RadioSFSENative.setFadeParams(ref, 1.5, 12.0, 8.0)
EndFunction

Function ResetFadeToIni(ObjectReference ref)
    RadioSFSENative.setFadeParams(ref, -1.0, -1.0, -1.0)
EndFunction
```

### `set_positions(ref, emitterX, emitterY, emitterZ, playerX, playerY, playerZ, playerYawDeg)`

Feeds the plugin the emitter/player positional sample used for distance fade and stereo pan.

If your mod wants moving or spatialized radio behavior, call this regularly while playback is active.

Typical pattern:

```psc
Function PushPositionSample(ObjectReference ref)
    Actor playerRef = Game.GetPlayer()
    if ref == None || playerRef == None
        return
    endif

    Float ex = ref.GetPositionX()
    Float ey = ref.GetPositionY()
    Float ez = ref.GetPositionZ()
    Float px = playerRef.GetPositionX()
    Float py = playerRef.GetPositionY()
    Float pz = playerRef.GetPositionZ()
    Float yaw = playerRef.GetAngleZ()

    RadioSFSENative.set_positions(ref, ex, ey, ez, px, py, pz, yaw)
EndFunction
```

## Recommended Wrapper Pattern

Do not scatter raw native calls all over your mod.

Instead:

1. Create one small quest script or utility script that wraps `RadioSFSENative`.
2. Centralize waits and `lastError()` checks there.
3. Expose your own higher-level functions like:
   - `PlayStationByName`
   - `CycleToNextStream`
   - `PauseRadio`
   - `ApplyInteriorFadeProfile`

This gives you one place to handle future native changes.

## Common Pitfalls

- Calling `play(ref)` before selecting a source.
- Assuming `change_playlist()` completed immediately.
- Treating `setTrack()` returning `true` as proof that the track exists and started.
- Assuming different refs are isolated devices in the current build.
- Forgetting to poll `lastError(ref)` after async commands.
- Spamming the same command faster than the native debounce window.
- Expecting `currentTrackBasename(ref)` to return a value while stopped.
- Expecting `getTrack(ref)` to return a normal filename for stream stations.

## Suggested Integration Recipes

### Recipe: Start a named station

```psc
Function StartNamedStation(ObjectReference ref, String stationName)
    if !RadioSFSENative.pluginAvailable(ref)
        return
    endif

    RadioSFSENative.change_playlist(ref, "Stations/" + stationName)
    Utility.Wait(0.25)
    RadioSFSENative.play(ref)
EndFunction
```

### Recipe: Switch to streaming mode

```psc
Function SwitchToStreaming(ObjectReference ref)
    if !RadioSFSENative.changeToNextSource(ref, 3)
        Debug.Trace("Could not switch to stream category: " + RadioSFSENative.lastError(ref))
        return
    endif

    Utility.Wait(0.25)
    Debug.Trace("Selected stream source: " + RadioSFSENative.currentSourceName(ref))
EndFunction
```

### Recipe: Cycle next source in current category and play

```psc
Function NextStationAndPlay(ObjectReference ref, Int category)
    if !RadioSFSENative.selectNextSource(ref, category)
        Debug.Trace("Next source failed: " + RadioSFSENative.lastError(ref))
        return
    endif

    Utility.Wait(0.25)
    RadioSFSENative.play(ref)
EndFunction
```

## File and Folder Expectations

By default the plugin scans:

- `Data/Radio/Playlists/<Name>`
- `Data/Radio/Stations/<Name>`
- `Data/Radio/FX`

Station folder naming rules:

- files starting with `transition_` are treated as transitions
- files starting with `ad_` are treated as ads
- other audio files are treated as songs

FX lookup accepts either the stem or the full filename.


