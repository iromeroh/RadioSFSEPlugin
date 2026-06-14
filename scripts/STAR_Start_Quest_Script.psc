Scriptname STAR_Start_Quest_Script extends Quest

; =================================================================================================
; CK setup: core radio forms and active emitter references
; =================================================================================================
; permanentRadioEmitter is the legacy placed/inventory radio reference used by older setup.
; RadioItemBaseForm is the preferred stable base form for ownership checks.
; PortableRadioForms can include additional portable radio item/base forms that share persistence.
; RadioEmitter tracks the currently active world/fixed/player emitter at runtime.
ObjectReference Property permanentRadioEmitter Auto Mandatory
Form Property RadioItemBaseForm Auto
FormList Property PortableRadioForms Auto
ObjectReference Property RadioEmitter Auto
String Property StartupPlaylist = "Default" Auto

; =================================================================================================
; CK setup: player control surfaces
; =================================================================================================
; playStopSlate is optional. If None, only weapon/keyboard/terminal controls are distributed.
; RadioControlTerminalRef is the persistent invisible terminal used by slate/weapon shortcuts.
; RadioShortcutWeapon is a favoriteable weapon used as a one-key menu opener.
; RadioHeadset is an equipped-only player radio source; carrying it alone does not play audio.
; RadioHeadsetForms can include additional headset armor forms that share persistence.
Book Property playStopSlate Auto  ; Drag your slate form here in CK
;Book Property forwardSlate Auto  ; Drag your slate form here in CK
;Book Property mediaTypeSlate Auto  ; Drag your slate form here in CK
;Book Property playlistSlate Auto  ; Drag your slate form here in CK
ObjectReference Property RadioControlTerminalRef Auto
Form Property RadioShortcutWeapon Auto
Int Property RadioShortcutWeaponFavoriteSlot = -1 Auto
Armor Property RadioHeadset Auto
FormList Property RadioHeadsetForms Auto
Bool Property HeadsetRadioEnabled = True Auto

; =================================================================================================
; CK setup: startup, access, and diagnostics
; =================================================================================================
Bool Property AutoStartPlayback = False Auto
Bool Property UseStationStart = False Auto
Bool Property UpdateFade = True Auto
Float Property FadeUpdateSeconds = 0.25 Auto
Bool Property RequireOwnedRadioForControls = True Auto
Bool Property GiveStarterRadioOnInit = False Auto
Bool Property VerboseNotifications = False Auto
Bool Property VerboseTraceLogs = False Auto
Bool Property NotifyTrackChangesOnTimer = True Auto

; =================================================================================================
; CK setup: fade, pan, and music/dialogue integration
; =================================================================================================
Bool Property UseCustomFadeParams = False Auto
Float Property FadeMinDistance = 0.1 Auto
Float Property FadeMaxDistance = 35.0 Auto
Float Property FadePanDistance = 5.0 Auto
Bool Property AutoDisableLegacyFadeOverride = True Auto
Bool Property MuteCellMusicWhileRadioPlaying = True Auto
MusicType Property MusicSilenceOverride Auto

; =================================================================================================
; Runtime state: access, active emitter location, persistence, notifications, and timers
; =================================================================================================
Bool Property RadioControlsUnlocked = False Auto Hidden
WorldSpace Property lastRadioWorldspace = None Auto
Cell Property lastRadioCell = None Auto Hidden
Bool Property HasPersistentState = False Auto Hidden
String Property PersistentSourceName = "" Auto Hidden
String Property PersistentTrackName = "" Auto Hidden
Float Property PersistentVolume = 60.0 Auto Hidden
ObjectReference Property LastTrackNotifyEmitter = None Auto Hidden
String Property LastTrackNotifiedName = "" Auto Hidden
Bool Property MusicSilenceActive = False Auto Hidden
Bool Property InDialogDuck = False Auto Hidden
Float Property PreDialogDuckVolume = -1.0 Auto Hidden
Bool Property PendingControlTerminalOpen = False Auto Hidden
Float Property ControlTerminalOpenDelay = 0.25 Auto

; =================================================================================================
; CK setup: distribution fallback mode (no SFSE plugin)
; =================================================================================================
; Distribution fallback mode (no SFSE plugin):
; - mediaType 2 (stations) can play CK-declared Wwise events.
; - mediaType 1/3 (local/stream) are SFSE-only and fall back to static.
Bool Property ShowSFSEMissingNotice = True Auto
Int Property FallbackAdIntervalSongs = 3 Auto
WwiseEvent Property FallbackStaticEvent Auto
WwiseEvent Property FallbackTuningShortEvent Auto
WwiseEvent Property FallbackTuningLongEvent Auto
WwiseEvent Property FallbackNotificationEvent Auto
WwiseEvent Property FallbackNoStationEvent Auto

; =================================================================================================
; CK setup: fallback station form lists
; =================================================================================================
FormList Property StationAkilaSongs Auto
FormList Property StationAkilaJingles Auto
FormList Property StationAkilaAds Auto
FormList Property StationNeonSongs Auto
FormList Property StationNeonJingles Auto
FormList Property StationNeonAds Auto
FormList Property StationAtlantisSongs Auto
FormList Property StationAtlantisJingles Auto
FormList Property StationAtlantisAds Auto
FormList Property StationHopetownSongs Auto
FormList Property StationHopetownJingles Auto
FormList Property StationHopetownAds Auto
FormList Property StationParadisoSongs Auto
FormList Property StationParadisoJingles Auto
FormList Property StationParadisoAds Auto

; =================================================================================================
; Runtime state: SFSE compatibility, fallback sequencing, and cached setup lookups
; =================================================================================================
Bool Property SFSEProbeDone = False Auto Hidden
Bool Property SFSEAvailable = False Auto Hidden
Bool Property SFSEMissingNoticeShown = False Auto Hidden
Int Property DebugVerbosityLevel = 0 Auto Hidden
String Property CompatLastError = "" Auto Hidden
Bool Property FallbackIsPlayingState = False Auto Hidden
Int Property FallbackCurrentInstanceId = 0 Auto Hidden
String Property FallbackCurrentEntryName = "" Auto Hidden
String Property FallbackCurrentSourceName = "Akila Country" Auto Hidden
Int Property FallbackStationIndex = 0 Auto Hidden
Int Property FallbackSongIndex = 0 Auto Hidden
Int Property FallbackJingleIndex = 0 Auto Hidden
Int Property FallbackAdIndex = 0 Auto Hidden
Int Property FallbackSongsSinceAd = 0 Auto Hidden
Bool Property FallbackPreviousWasSong = False Auto Hidden
Float Property FallbackVolume = 60.0 Auto Hidden
Form Property CachedRadioBaseForm = None Auto Hidden
Bool Property TriedLegacyPermanentBaseResolve = False Auto Hidden

; =================================================================================================
; Runtime constants and simple internal state
; =================================================================================================
int MyTimer = 528
int ControlTerminalTimer = 529
int mediaType = 1

Function SetCompatError(String errorText)
	CompatLastError = errorText
	if errorText != ""
		Trace("CompatError: " + errorText)
	endif
EndFunction

Function ClearCompatError()
	CompatLastError = ""
EndFunction

Bool Function IsSFSEAvailable(ObjectReference probeRef = None)
	if SFSEProbeDone
		return SFSEAvailable
	endif

	; Safe default so stale saved state cannot force native calls when plugin is missing.
	SFSEAvailable = False

	if probeRef == None
		probeRef = ResolveEmitterForState()
	endif
	if probeRef == None
		probeRef = Game.GetPlayer()
	endif

	Bool probeResult = RadioSFSENative.pluginAvailable(probeRef)
	if probeResult
		SFSEAvailable = True
	endif
	SFSEProbeDone = True

	if !SFSEAvailable && ShowSFSEMissingNotice && !SFSEMissingNoticeShown
		SFSEMissingNoticeShown = True
		Notify("RadioSFSE not detected. Station fallback mode enabled.", True)
	endif

	return SFSEAvailable
EndFunction

Int Function FallbackStationCount()
	return 5
EndFunction

Int Function ClampFallbackStationIndex(Int index)
	Int count = FallbackStationCount()
	if count <= 0
		return 0
	endif

	while index < 0
		index += count
	endwhile

	while index >= count
		index -= count
	endwhile
	return index
EndFunction

String Function FallbackStationName(Int index)
	index = ClampFallbackStationIndex(index)
	if index == 0
		return "Akila Country"
	elseif index == 1
		return "Neon Jazz"
	elseif index == 2
		return "Atlantis Electro"
	elseif index == 3
		return "Hopetown Oldies"
	endif
	return "Paradiso Tropicana"
EndFunction

FormList Function FallbackStationSongs(Int index)
	index = ClampFallbackStationIndex(index)
	if index == 0
		return StationAkilaSongs
	elseif index == 1
		return StationNeonSongs
	elseif index == 2
		return StationAtlantisSongs
	elseif index == 3
		return StationHopetownSongs
	endif
	return StationParadisoSongs
EndFunction

FormList Function FallbackStationJingles(Int index)
	index = ClampFallbackStationIndex(index)
	if index == 0
		return StationAkilaJingles
	elseif index == 1
		return StationNeonJingles
	elseif index == 2
		return StationAtlantisJingles
	elseif index == 3
		return StationHopetownJingles
	endif
	return StationParadisoJingles
EndFunction

FormList Function FallbackStationAds(Int index)
	index = ClampFallbackStationIndex(index)
	if index == 0
		return StationAkilaAds
	elseif index == 1
		return StationNeonAds
	elseif index == 2
		return StationAtlantisAds
	elseif index == 3
		return StationHopetownAds
	endif
	return StationParadisoAds
EndFunction

Function ResetFallbackStationSequence()
	FallbackSongIndex = 0
	FallbackJingleIndex = 0
	FallbackAdIndex = 0
	FallbackSongsSinceAd = 0
	FallbackPreviousWasSong = False
	FallbackCurrentEntryName = ""
EndFunction

Function SetFallbackStationIndex(Int index)
	FallbackStationIndex = ClampFallbackStationIndex(index)
	FallbackCurrentSourceName = FallbackStationName(FallbackStationIndex)
EndFunction

Int Function FindFallbackStationByName(String stationName)
	if stationName == ""
		return -1
	endif

	if stationName == "Akila Country" || stationName == "AkilaCountry"
		return 0
	elseif stationName == "Neon Jazz" || stationName == "NeonJazz"
		return 1
	elseif stationName == "Atlantis Electro" || stationName == "AtlantisElectro"
		return 2
	elseif stationName == "Hopetown Oldies" || stationName == "Hopetown oldies" || stationName == "HopetownOldies"
		return 3
	elseif stationName == "Paradiso Tropicana" || stationName == "ParadisoTropicana"
		return 4
	endif

	return -1
EndFunction

Function StopFallbackPlayback()
	if FallbackCurrentInstanceId > 0
		WwiseEvent.StopInstance(FallbackCurrentInstanceId)
	endif
	FallbackCurrentInstanceId = 0
	FallbackIsPlayingState = False
EndFunction

Bool Function PlayFallbackPrimaryEvent(ObjectReference emitterRef, WwiseEvent eventToPlay, String label)
	if emitterRef == None || eventToPlay == None
		SetCompatError("No fallback station sound is configured.")
		return false
	endif

	StopFallbackPlayback()
	Int instanceId = eventToPlay.Play(emitterRef)
	if instanceId <= 0
		SetCompatError("Could not play fallback station sound.")
		return false
	endif

	FallbackCurrentInstanceId = instanceId
	FallbackIsPlayingState = True
	FallbackCurrentEntryName = label
	return true
EndFunction

WwiseEvent Function ResolveFallbackFxEvent(String fxBasename)
	if fxBasename == "" 
		return None
	endif

	if fxBasename == "tuning_short" || fxBasename == "tuning_short.mp3"
		return FallbackTuningShortEvent
	elseif fxBasename == "tuning_long" || fxBasename == "tuning_long.mp3"
		return FallbackTuningLongEvent
	elseif fxBasename == "notification" || fxBasename == "notification.mp3"
		return FallbackNotificationEvent
	elseif fxBasename == "no_station" || fxBasename == "no_station.mp3"
		return FallbackNoStationEvent
	elseif fxBasename == "static" || fxBasename == "static.mp3"
		return FallbackStaticEvent
	endif

	return None
EndFunction

Form Function ChooseFallbackNextEntry()
	Int stationIndex = ClampFallbackStationIndex(FallbackStationIndex)
	FormList songs = FallbackStationSongs(stationIndex)
	if songs == None || songs.GetSize() <= 0
		return None
	endif

	FormList jingles = FallbackStationJingles(stationIndex)
	FormList ads = FallbackStationAds(stationIndex)
	Form chosenEntry = None
	Int songCount = songs.GetSize()

	if !FallbackPreviousWasSong
		if FallbackSongIndex >= songCount
			FallbackSongIndex = 0
		endif
		chosenEntry = songs.GetAt(FallbackSongIndex)
		FallbackSongIndex += 1
		if FallbackSongIndex >= songCount
			FallbackSongIndex = 0
		endif
		FallbackSongsSinceAd += 1
		FallbackPreviousWasSong = True
		return chosenEntry
	endif

	Int adCount = 0
	if ads != None
		adCount = ads.GetSize()
	endif

	Int jingleCount = 0
	if jingles != None
		jingleCount = jingles.GetSize()
	endif

	if adCount > 0 && FallbackAdIntervalSongs > 0 && FallbackSongsSinceAd >= FallbackAdIntervalSongs
		if FallbackAdIndex >= adCount
			FallbackAdIndex = 0
		endif
		chosenEntry = ads.GetAt(FallbackAdIndex)
		FallbackAdIndex += 1
		if FallbackAdIndex >= adCount
			FallbackAdIndex = 0
		endif
		FallbackSongsSinceAd = 0
		FallbackPreviousWasSong = False
		return chosenEntry
	endif

	if jingleCount > 0
		if FallbackJingleIndex >= jingleCount
			FallbackJingleIndex = 0
		endif
		chosenEntry = jingles.GetAt(FallbackJingleIndex)
		FallbackJingleIndex += 1
		if FallbackJingleIndex >= jingleCount
			FallbackJingleIndex = 0
		endif
		FallbackPreviousWasSong = False
		return chosenEntry
	endif

	if FallbackSongIndex >= songCount
		FallbackSongIndex = 0
	endif
	chosenEntry = songs.GetAt(FallbackSongIndex)
	FallbackSongIndex += 1
	if FallbackSongIndex >= songCount
		FallbackSongIndex = 0
	endif
	FallbackSongsSinceAd += 1
	FallbackPreviousWasSong = True
	return chosenEntry
EndFunction

Form Function ChooseFallbackPreviousSongEntry()
	Int stationIndex = ClampFallbackStationIndex(FallbackStationIndex)
	FormList songs = FallbackStationSongs(stationIndex)
	if songs == None
		return None
	endif

	Int songCount = songs.GetSize()
	if songCount <= 0
		return None
	endif

	FallbackSongIndex -= 1
	if FallbackSongIndex < 0
		FallbackSongIndex = songCount - 1
	endif

	FallbackPreviousWasSong = True
	Form chosenEntry = songs.GetAt(FallbackSongIndex)
	if chosenEntry != None
		FallbackSongsSinceAd += 1
	endif
	return chosenEntry
EndFunction

Bool Function RadioPlayFx(ObjectReference emitterRef, String fxBasename)
	if emitterRef == None || fxBasename == ""
		return false
	endif

	if IsSFSEAvailable(emitterRef)
		return RadioSFSENative.playFx(emitterRef, fxBasename)
	endif

	WwiseEvent fxEvent = ResolveFallbackFxEvent(fxBasename)
	if fxEvent == None
		return false
	endif
	return fxEvent.Play(emitterRef) > 0
EndFunction

Bool Function RadioSupportsNative(ObjectReference emitterRef = None)
	return IsSFSEAvailable(emitterRef)
EndFunction

Bool Function RadioIsPlaying(ObjectReference emitterRef)
	if IsSFSEAvailable(emitterRef)
		return RadioSFSENative.isPlaying(emitterRef)
	endif
	return FallbackIsPlayingState
EndFunction

String Function RadioLastError(ObjectReference emitterRef)
	if IsSFSEAvailable(emitterRef)
		return RadioSFSENative.lastError(emitterRef)
	endif
	return CompatLastError
EndFunction

String Function RadioCurrentSourceName(ObjectReference emitterRef)
	if IsSFSEAvailable(emitterRef)
		return RadioSFSENative.currentSourceName(emitterRef)
	endif

	if mediaType == 2
		return FallbackCurrentSourceName
	elseif mediaType == 1
		return "Local media (SFSE required)"
	elseif mediaType == 3
		return "Streaming media (SFSE required)"
	endif

	return ""
EndFunction

String Function RadioCurrentTrackBasename(ObjectReference emitterRef)
	if IsSFSEAvailable(emitterRef)
		return RadioSFSENative.currentTrackBasename(emitterRef)
	endif
	return FallbackCurrentEntryName
EndFunction

String Function RadioGetTrack(ObjectReference emitterRef)
	if IsSFSEAvailable(emitterRef)
		return RadioSFSENative.getTrack(emitterRef)
	endif
	return FallbackCurrentEntryName
EndFunction

Float Function RadioGetVolume(ObjectReference emitterRef)
	if IsSFSEAvailable(emitterRef)
		return RadioSFSENative.getVolume(emitterRef)
	endif
	return FallbackVolume
EndFunction

Int Function RadioGetMediaType(ObjectReference emitterRef = None)
	if emitterRef == None
		emitterRef = ResolveEmitterForControls()
	endif
	if emitterRef == None
		emitterRef = ResolveEmitterForState()
	endif

	if IsSFSEAvailable(emitterRef)
		Int nativeType = RadioSFSENative.getMediaType(emitterRef)
		if nativeType >= 1 && nativeType <= 3
			mediaType = nativeType
			return nativeType
		endif
	endif

	return mediaType
EndFunction

Int Function RadioGetPlayMode(ObjectReference emitterRef = None)
	if emitterRef == None
		emitterRef = ResolveEmitterForControls()
	endif
	if emitterRef == None
		emitterRef = ResolveEmitterForState()
	endif

	if IsSFSEAvailable(emitterRef)
		Int nativeMode = RadioSFSENative.getPlayMode(emitterRef)
		if nativeMode == 2
			return 2
		endif
		return 1
	endif

	return 1
EndFunction

Bool Function RadioSetPlayMode(ObjectReference emitterRef, Int playMode)
	if IsSFSEAvailable(emitterRef)
		return RadioSFSENative.setPlayMode(emitterRef, playMode)
	endif
	return false
EndFunction

String Function PlayModeName(Int playMode)
	if playMode == 2
		return "Shuffle"
	endif
	return "Alphabetic"
EndFunction

Bool Function RadioSetVolume(ObjectReference emitterRef, Float volume)
	if IsSFSEAvailable(emitterRef)
		return RadioSFSENative.setVolume(emitterRef, volume)
	endif

	if volume < 0.0
		volume = 0.0
	elseif volume > 200.0
		volume = 200.0
	endif

	FallbackVolume = volume
	ClearCompatError()
	return true
EndFunction

Float Function ResolveVolumeStepPercent(ObjectReference emitterRef, Float fallbackStep = 20.0)
	Float resolvedStep = fallbackStep
	if resolvedStep <= 0.0
		resolvedStep = 20.0
	endif

	if IsSFSEAvailable(emitterRef)
		Float nativeStep = RadioSFSENative.getVolumeStepPercent(emitterRef)
		if nativeStep > 0.0
			resolvedStep = nativeStep
		endif
	endif

	return resolvedStep
EndFunction

Bool Function RadioVolumeUp(ObjectReference emitterRef, Float step)
	if IsSFSEAvailable(emitterRef)
		return RadioSFSENative.volumeUp(emitterRef, step)
	endif

	if step <= 0.0
		step = 5.0
	endif
	return RadioSetVolume(emitterRef, FallbackVolume + step)
EndFunction

Bool Function RadioVolumeDown(ObjectReference emitterRef, Float step)
	if IsSFSEAvailable(emitterRef)
		return RadioSFSENative.volumeDown(emitterRef, step)
	endif

	if step <= 0.0
		step = 5.0
	endif
	return RadioSetVolume(emitterRef, FallbackVolume - step)
EndFunction

Bool Function RadioSetFadeParams(ObjectReference emitterRef, Float minDist, Float maxDist, Float panDist)
	if IsSFSEAvailable(emitterRef)
		return RadioSFSENative.setFadeParams(emitterRef, minDist, maxDist, panDist)
	endif
	return false
EndFunction

Bool Function RadioPlay(ObjectReference emitterRef)
	if emitterRef == None
		SetCompatError("No radio device.")
		return false
	endif

	if IsSFSEAvailable(emitterRef)
		Int category = RadioGetMediaType(emitterRef)
		if category < 1 || category > 3
			category = 1
		endif

		String sourceName = RadioSFSENative.currentSourceName(emitterRef)
		if sourceName == ""
			Bool selected = RadioSFSENative.changeToNextSource(emitterRef, category)
			if !selected
				SetCompatError("No media source available for current media type.")
				return false
			endif
		endif

		ClearCompatError()
		RadioSFSENative.play(emitterRef)
		return true
	endif

	if mediaType != 2
		SetCompatError("SFSE plugin required for local/stream playback. Playing static.")
		StopFallbackPlayback()
		RadioPlayFx(emitterRef, "static")
		return false
	endif

	Form nextEntry = ChooseFallbackNextEntry()
	WwiseEvent nextEvent = nextEntry as WwiseEvent
	if nextEvent == None
		SetCompatError("No fallback station tracks configured in CK.")
		return false
	endif

	String label = FallbackCurrentSourceName

	ClearCompatError()
	return PlayFallbackPrimaryEvent(emitterRef, nextEvent, label)
EndFunction

Bool Function RadioStart(ObjectReference emitterRef)
	if IsSFSEAvailable(emitterRef)
		RadioSFSENative.start(emitterRef)
		return true
	endif
	return RadioPlay(emitterRef)
EndFunction

Bool Function RadioPause(ObjectReference emitterRef)
	if IsSFSEAvailable(emitterRef)
		RadioSFSENative.pause(emitterRef)
		return true
	endif

	StopFallbackPlayback()
	ClearCompatError()
	return true
EndFunction

Bool Function RadioStop(ObjectReference emitterRef)
	if IsSFSEAvailable(emitterRef)
		RadioSFSENative.stop(emitterRef)
		return true
	endif

	StopFallbackPlayback()
	ClearCompatError()
	return true
EndFunction

Bool Function RadioForward(ObjectReference emitterRef)
	if emitterRef == None
		SetCompatError("No radio device.")
		return false
	endif

	if IsSFSEAvailable(emitterRef)
		RadioSFSENative.forward(emitterRef)
		return true
	endif

	if mediaType != 2
		SetCompatError("SFSE plugin required for local/stream playback. Playing static.")
		StopFallbackPlayback()
		RadioPlayFx(emitterRef, "static")
		return false
	endif

	Form nextEntry = ChooseFallbackNextEntry()
	WwiseEvent nextEvent = nextEntry as WwiseEvent
	if nextEvent == None
		SetCompatError("No fallback station tracks configured in CK.")
		return false
	endif

	String label = FallbackCurrentSourceName

	ClearCompatError()
	return PlayFallbackPrimaryEvent(emitterRef, nextEvent, label)
EndFunction

Bool Function RadioRewind(ObjectReference emitterRef)
	if emitterRef == None
		SetCompatError("No radio device.")
		return false
	endif

	if IsSFSEAvailable(emitterRef)
		RadioSFSENative.rewind(emitterRef)
		return true
	endif

	if mediaType != 2
		SetCompatError("SFSE plugin required for local/stream playback. Playing static.")
		StopFallbackPlayback()
		RadioPlayFx(emitterRef, "static")
		return false
	endif

	Form prevEntry = ChooseFallbackPreviousSongEntry()
	WwiseEvent prevEvent = prevEntry as WwiseEvent
	if prevEvent == None
		SetCompatError("No fallback station tracks configured in CK.")
		return false
	endif

	String label = FallbackCurrentSourceName

	ClearCompatError()
	return PlayFallbackPrimaryEvent(emitterRef, prevEvent, label)
EndFunction

Bool Function RadioPrevious(ObjectReference emitterRef)
	if emitterRef == None
		SetCompatError("No radio device.")
		return false
	endif

	if IsSFSEAvailable(emitterRef)
		; Robust previous behavior across plugin versions:
		; first rewind may restart current track if position > 3s.
		; if track name is unchanged after debounce window, run a second rewind to force previous track.
		String beforeTrack = RadioCurrentTrackBasename(emitterRef)
		RadioSFSENative.rewind(emitterRef)
		Utility.Wait(0.25)
		String afterTrack = RadioCurrentTrackBasename(emitterRef)
		if afterTrack == beforeTrack
			RadioSFSENative.rewind(emitterRef)
		endif
		return true
	endif

	if mediaType != 2
		SetCompatError("SFSE plugin required for local/stream playback. Playing static.")
		StopFallbackPlayback()
		RadioPlayFx(emitterRef, "static")
		return false
	endif

	Form prevEntry = ChooseFallbackPreviousSongEntry()
	WwiseEvent prevEvent = prevEntry as WwiseEvent
	if prevEvent == None
		SetCompatError("No fallback station tracks configured in CK.")
		return false
	endif

	String label = FallbackCurrentSourceName

	ClearCompatError()
	return PlayFallbackPrimaryEvent(emitterRef, prevEvent, label)
EndFunction

Bool Function RadioChangeToNextSource(ObjectReference emitterRef, Int category)
	if IsSFSEAvailable(emitterRef)
		return RadioSFSENative.changeToNextSource(emitterRef, category)
	endif

	if category != 2
		SetCompatError("SFSE plugin required for local and streaming source types.")
		return false
	endif

	SetFallbackStationIndex(0)
	ResetFallbackStationSequence()
	ClearCompatError()
	return true
EndFunction

Bool Function RadioSelectNextSource(ObjectReference emitterRef, Int category)
	if IsSFSEAvailable(emitterRef)
		return RadioSFSENative.selectNextSource(emitterRef, category)
	endif

	if category != 2
		SetCompatError("SFSE plugin required for local and streaming source types.")
		return false
	endif

	SetFallbackStationIndex(FallbackStationIndex + 1)
	ResetFallbackStationSequence()
	ClearCompatError()
	return true
EndFunction

Bool Function RadioChangePlaylist(ObjectReference emitterRef, String sourceName)
	if IsSFSEAvailable(emitterRef)
		RadioSFSENative.change_playlist(emitterRef, sourceName)
		return true
	endif

	Int index = FindFallbackStationByName(sourceName)
	if index < 0
		SetCompatError("Station not found in fallback set: " + sourceName)
		return false
	endif

	SetFallbackStationIndex(index)
	ResetFallbackStationSequence()
	ClearCompatError()
	return true
EndFunction

Bool Function RadioSetTrack(ObjectReference emitterRef, String trackName)
	if IsSFSEAvailable(emitterRef)
		return RadioSFSENative.setTrack(emitterRef, trackName)
	endif

	; Fallback station mode uses Wwise events; direct track-name lookup is optional.
	return false
EndFunction

Function Notify(String text, Bool force = False)
	if text == ""
		return
	endif
	if force || VerboseNotifications
		Debug.Notification(text)
	endif
EndFunction

Function Trace(String text)
	if !VerboseTraceLogs
		return
	endif
	Debug.Trace("STAR_Radio: " + text)
EndFunction

Function EnsureDiagnosticsEnabled()
	Int verbosity = 0
	if IsSFSEAvailable(RadioEmitter)
		verbosity = RadioSFSENative.getDebugVerbosity(RadioEmitter)
	endif

	if verbosity < 0
		verbosity = 0
	elseif verbosity > 2
		verbosity = 2
	endif

	DebugVerbosityLevel = verbosity
	VerboseTraceLogs = verbosity > 0
EndFunction

Function NormalizeFadeConfig()
	if !AutoDisableLegacyFadeOverride
		return
	endif

	; Legacy quest property values that unintentionally override INI fade settings.
	if UseCustomFadeParams
		if FadeMinDistance >= 149.0 && FadeMinDistance <= 151.0
			if FadeMaxDistance >= 4990.0 && FadeMaxDistance <= 5010.0
				if FadePanDistance >= 499.0 && FadePanDistance <= 501.0
					UseCustomFadeParams = False
					Trace("NormalizeFadeConfig: legacy custom fade override disabled; using INI values.")
				endif
			endif
		endif
	endif
EndFunction

Event OnInit()
	ResetSFSEProbeState()
	RefreshControlSlateAccess()

	InitializeRadio()
EndEvent

Function ResetSFSEProbeState()
	SFSEProbeDone = False
	SFSEAvailable = False
	if mediaType < 1 || mediaType > 3
		mediaType = 1
	endif
	if !IsSFSEAvailable(RadioEmitter)
		mediaType = 2
	endif
	EnsureDiagnosticsEnabled()
	SetFallbackStationIndex(FallbackStationIndex)
EndFunction

Form Function RadioBaseForm()
	if CachedRadioBaseForm != None
		return CachedRadioBaseForm
	endif

	; Preferred stable source: explicit base form set in CK.
	if RadioItemBaseForm != None
		CachedRadioBaseForm = RadioItemBaseForm
		return CachedRadioBaseForm
	endif

	; Legacy compatibility: try to resolve from permanent emitter only once.
	if !TriedLegacyPermanentBaseResolve && permanentRadioEmitter != None
		TriedLegacyPermanentBaseResolve = True
		Form legacyBase = permanentRadioEmitter.GetBaseObject()
		if legacyBase != None
			CachedRadioBaseForm = legacyBase
			return CachedRadioBaseForm
		endif
	endif

	return None
EndFunction

int Function GetCarriedRadioCount()
	Actor player = Game.GetPlayer()
	if player == None
		return 0
	endif

	int count = 0
	Form radioBase = RadioBaseForm()
	if radioBase != None
		count += player.GetItemCount(radioBase)
	endif

	if PortableRadioForms != None
		int i = 0
		int listCount = PortableRadioForms.GetSize()
		while i < listCount
			Form portableForm = PortableRadioForms.GetAt(i)
			if portableForm != None && portableForm != radioBase
				count += player.GetItemCount(portableForm)
			endif
			i += 1
		endwhile
	endif

	return count
EndFunction

int Function GetCarriedHeadsetCount()
	Actor player = Game.GetPlayer()
	if player == None
		return 0
	endif

	int count = 0
	if RadioHeadset != None
		count += player.GetItemCount(RadioHeadset)
	endif

	if RadioHeadsetForms != None
		int i = 0
		int listCount = RadioHeadsetForms.GetSize()
		while i < listCount
			Form headsetForm = RadioHeadsetForms.GetAt(i)
			if headsetForm != None && headsetForm != RadioHeadset
				count += player.GetItemCount(headsetForm)
			endif
			i += 1
		endwhile
	endif

	return count
EndFunction

Bool Function IsRadioHeadsetEquipped()
	Actor player = Game.GetPlayer()
	if !HeadsetRadioEnabled || player == None
		return false
	endif

	if RadioHeadset != None && player.IsEquipped(RadioHeadset)
		return true
	endif

	if RadioHeadsetForms != None
		int i = 0
		int listCount = RadioHeadsetForms.GetSize()
		while i < listCount
			Form headsetForm = RadioHeadsetForms.GetAt(i)
			if headsetForm != None && headsetForm != RadioHeadset && player.IsEquipped(headsetForm)
				return true
			endif
			i += 1
		endwhile
	endif

	return false
EndFunction

Bool Function IsPortableRadioForm(Form itemForm)
	if itemForm == None
		return false
	endif

	Form radioBase = RadioBaseForm()
	if radioBase != None && itemForm == radioBase
		return true
	endif

	return PortableRadioForms != None && PortableRadioForms.HasForm(itemForm)
EndFunction

Bool Function IsRadioHeadsetForm(Form itemForm)
	if itemForm == None
		return false
	endif

	if RadioHeadset != None && itemForm == RadioHeadset
		return true
	endif

	return RadioHeadsetForms != None && RadioHeadsetForms.HasForm(itemForm)
EndFunction

Bool Function HasPlayerRadioEmitter()
	return GetCarriedRadioCount() > 0 || IsRadioHeadsetEquipped()
EndFunction

Function SyncPlayerDeviceClass()
	Actor player = Game.GetPlayer()
	if player == None
		return
	endif

	if IsRadioHeadsetEquipped()
		RadioSFSENative.notifyDeviceClass(player, 2)
	else
		RadioSFSENative.notifyDeviceClass(player, 0)
	endif
EndFunction

Function ForceStopPlayerRadioSlots()
	Actor player = Game.GetPlayer()
	if player == None
		return
	endif

	ObjectReference playerRef = player as ObjectReference
	RadioSFSENative.notifyDeviceClass(playerRef, 0)
	CapturePersistentState(playerRef)
	RadioStop(playerRef)

	RadioSFSENative.notifyDeviceClass(playerRef, 2)
	CapturePersistentState(playerRef)
	RadioStop(playerRef)

	StopFallbackPlayback()
EndFunction

Bool Function canUseRadioControls()
	if !RequireOwnedRadioForControls
		return True
	endif
	return RadioControlsUnlocked
EndFunction

Function UpdateLastRadioLocation(ObjectReference locationRef)
	if locationRef == None
		lastRadioWorldspace = None
		lastRadioCell = None
		Trace("UpdateLastRadioLocation: cleared.")
		return
	endif

	lastRadioWorldspace = locationRef.GetWorldSpace()
	if lastRadioWorldspace == None
		lastRadioCell = locationRef.GetParentCell()
	else
		lastRadioCell = None
	endif
	Trace("UpdateLastRadioLocation: ref=" + locationRef + " ws=" + lastRadioWorldspace + " cell=" + lastRadioCell)
EndFunction

Bool Function IsReferenceReachableFromPlayer(ObjectReference ref)
	Actor player = Game.GetPlayer()
	if player == None || ref == None
		return false
	endif

	if ref == player
		return HasPlayerRadioEmitter()
	endif

	WorldSpace playerWS = player.GetWorldSpace()
	WorldSpace refWS = ref.GetWorldSpace()
	if refWS != playerWS
		return false
	endif

	if playerWS == None
		Cell playerCell = player.GetParentCell()
		Cell refCell = ref.GetParentCell()
		if refCell != playerCell
			return false
		endif
	endif

	return true
EndFunction

Bool Function IsRadioReachableFromPlayer()
	ObjectReference emitterRef = RadioEmitter
	if emitterRef == None
		emitterRef = ResolveEmitterForControls()
	endif
	return IsEmitterReachableFromPlayer(emitterRef)
EndFunction

Bool Function IsEmitterReachableFromPlayer(ObjectReference emitterRef)
	Actor player = Game.GetPlayer()
	if player == None || emitterRef == None
		Trace("IsEmitterReachableFromPlayer: false (missing player or emitter).")
		return false
	endif

	if emitterRef == player
		Bool reachableOnPlayer = HasPlayerRadioEmitter()
		if !reachableOnPlayer
			Trace("IsEmitterReachableFromPlayer: false (emitter=player, no portable radio or equipped headset).")
		endif
		return reachableOnPlayer
	endif

	Bool reachable = IsReferenceReachableFromPlayer(emitterRef)
	if !reachable
		Trace("IsEmitterReachableFromPlayer: false (world/cell mismatch) emitter=" + emitterRef)
	endif
	return reachable
EndFunction

Bool Function IsTrackedEmitterStillReachable(ObjectReference emitterRef)
	Actor player = Game.GetPlayer()
	if player == None || emitterRef == None
		return false
	endif

	if emitterRef == player
		return IsEmitterReachableFromPlayer(emitterRef)
	endif

	if emitterRef != RadioEmitter
		return IsEmitterReachableFromPlayer(emitterRef)
	endif

	if lastRadioWorldspace != None
		Bool sameWorldspace = player.GetWorldSpace() == lastRadioWorldspace
		if !sameWorldspace
			Trace("IsTrackedEmitterStillReachable: false (stored worldspace mismatch) emitter=" + emitterRef)
		endif
		return sameWorldspace
	endif

	if lastRadioCell != None
		if player.GetWorldSpace() != None
			Trace("IsTrackedEmitterStillReachable: false (player moved from interior cell to worldspace) emitter=" + emitterRef)
			return false
		endif

		Bool sameCell = player.GetParentCell() == lastRadioCell
		if !sameCell
			Trace("IsTrackedEmitterStillReachable: false (stored cell mismatch) emitter=" + emitterRef)
		endif
		return sameCell
	endif

	return IsEmitterReachableFromPlayer(emitterRef)
EndFunction

Bool Function IsEmitterReferenceInvalid(ObjectReference emitterRef)
	if emitterRef == None
		return true
	endif

	Actor player = Game.GetPlayer()
	if player != None && emitterRef == player
		return false
	endif

	if emitterRef.IsDeleted()
		Trace("IsEmitterReferenceInvalid: deleted emitter " + emitterRef)
		return true
	endif

	if emitterRef.IsDestroyed()
		Trace("IsEmitterReferenceInvalid: destroyed emitter " + emitterRef)
		return true
	endif

	if emitterRef.IsDisabled()
		Trace("IsEmitterReferenceInvalid: disabled emitter " + emitterRef)
		return true
	endif

	return false
EndFunction

Function SanitizeActiveEmitter()
	ObjectReference activeEmitter = RadioEmitter
	if activeEmitter == None
		return
	endif

	if !IsEmitterReferenceInvalid(activeEmitter)
		return
	endif

	Trace("SanitizeActiveEmitter: clearing active emitter " + activeEmitter)
	if RadioIsPlaying(activeEmitter)
		RadioStop(activeEmitter)
	endif

	ResetTrackChangeNotification(activeEmitter)
	RadioEmitter = None
	lastRadioWorldspace = None
	lastRadioCell = None
EndFunction

Function ResetTrackChangeNotification(ObjectReference emitterRef = None)
	if emitterRef == None || LastTrackNotifyEmitter == emitterRef
		LastTrackNotifyEmitter = None
		LastTrackNotifiedName = ""
	endif
EndFunction

Function NotifyTrackChangeIfNeeded(ObjectReference emitterRef)
	if !NotifyTrackChangesOnTimer
		return
	endif
	if emitterRef == None
		return
	endif

	; Track-change notifications are only for local playlists/stations (1/2), not streams (3).
	Int currentMediaType = RadioGetMediaType(emitterRef)
	if currentMediaType != 1 && currentMediaType != 2
		ResetTrackChangeNotification(emitterRef)
		return
	endif

	String nowTrack = RadioCurrentTrackBasename(emitterRef)
	if nowTrack == "" || nowTrack == "na"
		return
	endif

	if LastTrackNotifyEmitter == emitterRef && LastTrackNotifiedName == nowTrack
		return
	endif

	LastTrackNotifyEmitter = emitterRef
	LastTrackNotifiedName = nowTrack
	Notify("Now playing: " + nowTrack, True)
EndFunction

Function EnsureControlSlateInventory(Bool shouldHave)
	Actor player = Game.GetPlayer()
	if player == None
		return
	endif

	if shouldHave
		if playStopSlate != None && player.GetItemCount(playStopSlate) == 0
			player.AddItem(playStopSlate, 1, true)
		endif
		if RadioShortcutWeapon != None && player.GetItemCount(RadioShortcutWeapon) == 0
			player.AddItem(RadioShortcutWeapon, 1, true)
			if RadioShortcutWeaponFavoriteSlot >= 0
				player.MarkItemAsFavorite(RadioShortcutWeapon, RadioShortcutWeaponFavoriteSlot)
			endif
		endif
		return
	endif

	if playStopSlate != None
		int c0 = player.GetItemCount(playStopSlate)
		if c0 > 0
			player.RemoveItem(playStopSlate, c0, true)
		endif
	endif
	if RadioShortcutWeapon != None
		int c1 = player.GetItemCount(RadioShortcutWeapon)
		if c1 > 0
			player.RemoveItem(RadioShortcutWeapon, c1, true)
		endif
	endif
EndFunction

Function RefreshControlSlateAccess()
	Actor player = Game.GetPlayer()
	if player == None
		return
	endif

	Form radioBase = RadioBaseForm()
	if GiveStarterRadioOnInit && !RadioControlsUnlocked && radioBase != None && player.GetItemCount(radioBase) == 0
		player.AddItem(radioBase, 1, true)
	endif

	if HasPlayerRadioEmitter() || GetCarriedHeadsetCount() > 0
		RadioControlsUnlocked = True
	endif

	Bool shouldHaveSlates = !RequireOwnedRadioForControls || RadioControlsUnlocked
	EnsureControlSlateInventory(shouldHaveSlates)
EndFunction

ObjectReference Function getEmitter()
	return RadioEmitter
EndFunction

ObjectReference Function ResolveEmitterForControls()
	SanitizeActiveEmitter()

	Actor player = Game.GetPlayer()
	if player == None
		return None
	endif

	Bool hasPlayerEmitter = HasPlayerRadioEmitter()
	ObjectReference mainEmitter = RadioEmitter

	if mainEmitter != None && mainEmitter != player
		if IsReferenceReachableFromPlayer(mainEmitter)
			return mainEmitter
		endif

		if hasPlayerEmitter
			SyncPlayerDeviceClass()
			return player
		endif

		return None
	endif

	if hasPlayerEmitter
		SyncPlayerDeviceClass()
		return player
	endif

	return None
EndFunction

ObjectReference Function ResolveEmitterForState()
	ObjectReference emitterRef = getEmitter()
	if emitterRef == None
		emitterRef = Game.GetPlayer()
	endif
	return emitterRef
EndFunction

Function setEmitter(ObjectReference ref)
        if ref == None
		return
	endif
       	RadioEmitter = ref
        
EndFunction

Function NotifyNoShortcutEmitter()
	if GetCarriedRadioCount() <= 0 && GetCarriedHeadsetCount() > 0 && !IsRadioHeadsetEquipped()
		Notify("Equip the radio headset to use it.", True)
	elseif GetCarriedRadioCount() > 0 || canUseRadioControls()
		Notify("No active radio device.", True)
	else
		Notify("Build or buy a radio first.", True)
	endif
EndFunction

Function OnRadioHeadsetEquipped()
	if !HeadsetRadioEnabled
		return
	endif

	Actor player = Game.GetPlayer()
	if player == None
		return
	endif

	ObjectReference playerRef = player as ObjectReference
	ObjectReference previousEmitter = getEmitter()

	if previousEmitter != None && previousEmitter != playerRef
		CapturePersistentState(previousEmitter)
		RadioStop(previousEmitter)
	endif

	; Stop any player-radio slot before switching the same player ref to headset.
	ForceStopPlayerRadioSlots()
	RadioSFSENative.notifyDeviceClass(playerRef, 2)

	RadioControlsUnlocked = True
	setEmitter(playerRef)
	lastRadioWorldspace = None
	lastRadioCell = None
	ApplyFadeParams(playerRef)
	CapturePersistentState(playerRef)
	SyncCellMusicMute(playerRef)
EndFunction

Function OnRadioHeadsetUnequipped()
	if !HeadsetRadioEnabled
		return
	endif

	Actor player = Game.GetPlayer()
	if player == None
		return
	endif

	ObjectReference playerRef = player as ObjectReference
	ForceStopPlayerRadioSlots()

	; After the headset is off, player-ref controls should address the portable radio slot.
	RadioSFSENative.notifyDeviceClass(playerRef, 0)

	if RadioEmitter == playerRef
		RadioEmitter = None
		lastRadioWorldspace = None
		lastRadioCell = None
	endif

	ResetTrackChangeNotification(playerRef)
	SyncCellMusicMute(playerRef)
EndFunction

Function DeactivateHeadsetForExternalRadio()
	if !HeadsetRadioEnabled
		return
	endif

	Actor player = Game.GetPlayer()
	if player == None
		return
	endif

	if IsRadioHeadsetEquipped()
		ForceStopPlayerRadioSlots()
		if RadioHeadset != None && player.IsEquipped(RadioHeadset)
			player.UnequipItem(RadioHeadset, false, true)
		endif
		if RadioHeadsetForms != None
			int i = 0
			int listCount = RadioHeadsetForms.GetSize()
			while i < listCount
				Form headsetForm = RadioHeadsetForms.GetAt(i)
				if headsetForm != None && headsetForm != RadioHeadset && player.IsEquipped(headsetForm)
					player.UnequipItem(headsetForm, false, true)
				endif
				i += 1
			endwhile
		endif
		RadioSFSENative.notifyDeviceClass(player, 0)
	endif
EndFunction

Function RequestOpenControlTerminal(Float delaySeconds = -1.0)
	if PendingControlTerminalOpen
		return
	endif

	if RadioControlTerminalRef == None
		Notify("Radio terminal menu not configured.", True)
		return
	endif

	if !canUseRadioControls()
		Notify("Build or buy a radio first.", True)
		return
	endif

	ObjectReference emitterRef = ResolveEmitterForControls()
	if emitterRef == None
		NotifyNoShortcutEmitter()
		return
	endif

	if !IsEmitterReachableFromPlayer(emitterRef)
		Notify("Radio is not here.", True)
		return
	endif

	if delaySeconds < 0.0
		delaySeconds = ControlTerminalOpenDelay
	endif
	if delaySeconds < 0.01
		delaySeconds = 0.01
	endif

	PendingControlTerminalOpen = True
	StartTimer(delaySeconds, ControlTerminalTimer)
EndFunction

Function OpenControlTerminalNow()
	PendingControlTerminalOpen = False

	Actor player = Game.GetPlayer()
	if player == None || RadioControlTerminalRef == None
		return
	endif

	RadioControlTerminalRef.MoveTo(player)
	RadioControlTerminalRef.Activate(player)
EndFunction

Bool Function PlayShortcutFxIfReachable(ObjectReference emitterRef, String fxBasename)
	if emitterRef == None || fxBasename == ""
		return false
	endif
	if !IsEmitterReachableFromPlayer(emitterRef)
		return false
	endif
	return RadioPlayFx(emitterRef, fxBasename)
EndFunction

Function StopShortcutFx(ObjectReference emitterRef)
	if emitterRef == None
		return
	endif
	if IsSFSEAvailable(emitterRef)
		RadioSFSENative.stopFx(emitterRef)
	endif
EndFunction

Bool Function WaitForShortcutPlaybackResult(ObjectReference emitterRef, Float timeoutSeconds = 6.0, Float stepSeconds = 0.25)
	Float elapsed = 0.0
	while elapsed < timeoutSeconds
		if RadioIsPlaying(emitterRef)
			return true
		endif

		String errTick = RadioLastError(emitterRef)
		if errTick != ""
			return false
		endif

		Utility.Wait(stepSeconds)
		elapsed += stepSeconds
	endwhile

	return RadioIsPlaying(emitterRef)
EndFunction

ObjectReference Function PrepareEmitterForShortcut()
	ObjectReference emitterRef = ResolveEmitterForControls()
	if emitterRef == None
		return None
	endif

	Actor player = Game.GetPlayer()
	ObjectReference previousEmitter = RadioEmitter
	if previousEmitter != emitterRef
		if previousEmitter != None
			CapturePersistentState(previousEmitter)
		endif

		setEmitter(emitterRef)
		if emitterRef == player
			lastRadioWorldspace = None
			lastRadioCell = None
		else
			UpdateLastRadioLocation(emitterRef)
		endif

		if !RadioIsPlaying(emitterRef)
			ApplyPersistentStateToEmitter(emitterRef)
		else
			ApplyFadeParams(emitterRef)
		endif
	endif

	return emitterRef
EndFunction

String Function ShortcutNowPlayingLabel(ObjectReference emitterRef)
	String label = RadioCurrentTrackBasename(emitterRef)
	if label == ""
		label = RadioCurrentSourceName(emitterRef)
	endif
	return label
EndFunction

Function ShortcutNextSource()
	ObjectReference emitterRef = PrepareEmitterForShortcut()
	if emitterRef == None
		NotifyNoShortcutEmitter()
		return
	endif

	Int category = RadioGetMediaType(emitterRef)
	if category == 2 || category == 3
		; Notify("Changing source...", True)
		PlayShortcutFxIfReachable(emitterRef, "tuning_short.mp3")
	else
		; Notify("Changing source...", True)
		PlayShortcutFxIfReachable(emitterRef, "notification.mp3")
	endif

	Bool selected = RadioSelectNextSource(emitterRef, category)
	StopShortcutFx(emitterRef)
	if !selected
		String errText = RadioLastError(emitterRef)
		if errText == ""
			errText = "Could not change playlist/source."
		endif
		Notify(errText, True)
	else
		String sourceName = RadioCurrentSourceName(emitterRef)
		Notify("Selected: " + sourceName, True)
	endif

	CapturePersistentState(emitterRef)
EndFunction

Function ShortcutPlayPause()
	ObjectReference emitterRef = PrepareEmitterForShortcut()
	if emitterRef == None
		NotifyNoShortcutEmitter()
		return
	endif

	if RadioIsPlaying(emitterRef)
		RadioPause(emitterRef)
		StopShortcutFx(emitterRef)
		String errPause = RadioLastError(emitterRef)
		if errPause != ""
			Notify(errPause, True)
		else
			Notify("Playback paused.")
		endif
		CapturePersistentState(emitterRef)
		return
	endif

	Int shortcutMediaType = RadioGetMediaType(emitterRef)
	if shortcutMediaType == 3
		Notify("Starting stream...", True)
		PlayShortcutFxIfReachable(emitterRef, "tuning_long.mp3")
	else
		Notify("Starting playback...", True)
	endif

	RadioPlay(emitterRef)
	Bool started = WaitForShortcutPlaybackResult(emitterRef)
	StopShortcutFx(emitterRef)
	if started
		Notify("Now playing: " + ShortcutNowPlayingLabel(emitterRef), True)
	else
		String errText = RadioLastError(emitterRef)
		if errText == ""
			errText = "Could not start playback."
		endif
		PlayShortcutFxIfReachable(emitterRef, "no_station.mp3")
		Notify(errText, True)
	endif

	CapturePersistentState(emitterRef)
EndFunction

Function ShortcutForward()
	ObjectReference emitterRef = PrepareEmitterForShortcut()
	if emitterRef == None
		NotifyNoShortcutEmitter()
		return
	endif

	RadioForward(emitterRef)
	String errText = RadioLastError(emitterRef)
	if errText != "" && !RadioIsPlaying(emitterRef)
		Utility.Wait(0.3)
		RadioForward(emitterRef)
		errText = RadioLastError(emitterRef)
	endif

	if errText != ""
		Notify(errText, True)
	else
		Notify("Now playing: " + ShortcutNowPlayingLabel(emitterRef), True)
	endif

	CapturePersistentState(emitterRef)
EndFunction

Function ShortcutRewind()
	ObjectReference emitterRef = PrepareEmitterForShortcut()
	if emitterRef == None
		NotifyNoShortcutEmitter()
		return
	endif

	RadioRewind(emitterRef)
	String errText = RadioLastError(emitterRef)
	if errText != "" && !RadioIsPlaying(emitterRef)
		Utility.Wait(0.3)
		RadioRewind(emitterRef)
		errText = RadioLastError(emitterRef)
	endif

	if errText != ""
		Notify(errText, True)
	else
		Notify("Now playing: " + ShortcutNowPlayingLabel(emitterRef), True)
	endif

	CapturePersistentState(emitterRef)
EndFunction

Function ShortcutVolumeUp()
	ObjectReference emitterRef = PrepareEmitterForShortcut()
	if emitterRef == None
		NotifyNoShortcutEmitter()
		return
	endif

	; Notify("Increasing volume...", True)
	PlayShortcutFxIfReachable(emitterRef, "notification.mp3")
	Float volumeStepNow = ResolveVolumeStepPercent(emitterRef, 20.0)
	Bool volUpOk = RadioVolumeUp(emitterRef, volumeStepNow)
	StopShortcutFx(emitterRef)
	if !volUpOk
		String errText = RadioLastError(emitterRef)
		if errText == ""
			errText = "Could not increase volume."
		endif
		Notify(errText, True)
	else
		Notify("Volume: " + RadioGetVolume(emitterRef), True)
	endif

	CapturePersistentState(emitterRef)
EndFunction

Function ShortcutVolumeDown()
	ObjectReference emitterRef = PrepareEmitterForShortcut()
	if emitterRef == None
		NotifyNoShortcutEmitter()
		return
	endif

	; Notify("Decreasing volume...", True)
	PlayShortcutFxIfReachable(emitterRef, "notification.mp3")
	Float volumeStepNow = ResolveVolumeStepPercent(emitterRef, 20.0)
	Bool volDownOk = RadioVolumeDown(emitterRef, volumeStepNow)
	StopShortcutFx(emitterRef)
	if !volDownOk
		String errText = RadioLastError(emitterRef)
		if errText == ""
			errText = "Could not decrease volume."
		endif
		Notify(errText, True)
	else
		Notify("Volume: " + RadioGetVolume(emitterRef), True)
	endif

	CapturePersistentState(emitterRef)
EndFunction

Function ApplyFadeParams(ObjectReference emitterRef)
	if emitterRef == None
		return
	endif

	NormalizeFadeConfig()

	if !UseCustomFadeParams
		RadioSetFadeParams(emitterRef, -1.0, -1.0, -1.0)
		Trace("ApplyFadeParams: using INI defaults for emitter " + emitterRef)
		return
	endif

	Float minDist = FadeMinDistance
	Float maxDist = FadeMaxDistance
	Float panDist = FadePanDistance

	if minDist < 0.0
		minDist = 0.1
	endif

	if maxDist <= minDist
		maxDist = minDist + 35.0
	endif

	if panDist <= 0.0
		panDist = 40.0
	endif

	RadioSetFadeParams(emitterRef, minDist, maxDist, panDist)
	Trace("ApplyFadeParams: custom min=" + minDist + " max=" + maxDist + " pan=" + panDist + " emitter=" + emitterRef)
EndFunction

Function SetMusicSilenceActive(Bool shouldBeActive)
	if MusicSilenceOverride == None
		MusicSilenceActive = False
		return
	endif

	if shouldBeActive
		if !MusicSilenceActive
			MusicSilenceOverride.Add()
			MusicSilenceActive = True
			Trace("Music silence override added.")
		endif
	else
		if MusicSilenceActive
			MusicSilenceOverride.Remove()
			MusicSilenceActive = False
			Trace("Music silence override removed.")
		endif
	endif
EndFunction

Function SyncCellMusicMute(ObjectReference emitterRef = None)
	if !MuteCellMusicWhileRadioPlaying
		SetMusicSilenceActive(False)
		return
	endif

	if MusicSilenceOverride == None
		return
	endif

	ObjectReference targetEmitter = emitterRef
	if targetEmitter == None
		targetEmitter = ResolveEmitterForControls()
	endif

	Bool shouldMute = False
	if targetEmitter != None && RadioIsPlaying(targetEmitter)
		shouldMute = IsEmitterReachableFromPlayer(targetEmitter)
	else
		Actor playerRef = Game.GetPlayer()
		if playerRef != None && RadioIsPlaying(playerRef)
			shouldMute = True
		elseif RadioEmitter != None && RadioIsPlaying(RadioEmitter)
			shouldMute = IsEmitterReachableFromPlayer(RadioEmitter)
		endif
	endif

	SetMusicSilenceActive(shouldMute)
EndFunction

Function ApplyPersistentStateToEmitter(ObjectReference emitterRef)
    
	return  ; does nothing

	if emitterRef == None
		return
	endif

	
	if IsSFSEAvailable(emitterRef)
		mediaType = RadioGetMediaType(emitterRef)
		String nativeSource = RadioCurrentSourceName(emitterRef)
		if nativeSource == "" && HasPersistentState
			RadioChangeToNextSource(emitterRef, mediaType)

			String sourceToApplyNative = PersistentSourceName
			if sourceToApplyNative == ""
				sourceToApplyNative = StartupPlaylist
			endif
			if sourceToApplyNative != ""
				RadioChangePlaylist(emitterRef, sourceToApplyNative)
			endif

			if PersistentTrackName != "" && PersistentTrackName != "na"
				RadioSetTrack(emitterRef, PersistentTrackName)
			endif

			RadioSetVolume(emitterRef, PersistentVolume)
		endif

		ApplyFadeParams(emitterRef)
		return
	endif

	RadioChangeToNextSource(emitterRef, mediaType)

	String sourceToApply = PersistentSourceName
	if sourceToApply == ""
		sourceToApply = StartupPlaylist
	endif
	if sourceToApply != ""
		RadioChangePlaylist(emitterRef, sourceToApply)
	endif

	if PersistentTrackName != "" && PersistentTrackName != "na"
		RadioSetTrack(emitterRef, PersistentTrackName)
	endif

	RadioSetVolume(emitterRef, PersistentVolume)
	ApplyFadeParams(emitterRef)
EndFunction

Function CapturePersistentState(ObjectReference emitterRef = None)
    return ; does nothing
	
	if emitterRef == None
		emitterRef = ResolveEmitterForState()
	endif
	if emitterRef == None
		return
	endif

	if IsSFSEAvailable(emitterRef)
		mediaType = RadioGetMediaType(emitterRef)
	endif

	String sourceName = RadioCurrentSourceName(emitterRef)
	if sourceName != ""
		PersistentSourceName = sourceName
		HasPersistentState = True
	endif

	String trackName = RadioGetTrack(emitterRef)
	if trackName != ""
		PersistentTrackName = trackName
	endif

	Float volumeNow = RadioGetVolume(emitterRef)
	if volumeNow >= 0.0
		PersistentVolume = volumeNow
		HasPersistentState = True
	endif
EndFunction

Function RestorePersistentState(ObjectReference emitterRef = None)
	if emitterRef == None
		emitterRef = ResolveEmitterForState()
	endif
	if emitterRef == None
		return
	endif

	setEmitter(emitterRef)
	if emitterRef == Game.GetPlayer()
		lastRadioWorldspace = None
		lastRadioCell = None
	else
		UpdateLastRadioLocation(emitterRef)
	endif

	ApplyPersistentStateToEmitter(emitterRef)
	CapturePersistentState(emitterRef)
EndFunction

Function setMediaType(int type, ObjectReference targetEmitter = None)
	if type < 1 || type > 3
		return
	endif
	
	ObjectReference emitterRef = targetEmitter
	if emitterRef == None
		emitterRef = ResolveEmitterForControls()
	endif
	if emitterRef == None
		return
	endif
	
	mediaType = type
	RadioPlayFx(emitterRef, "tuning_short")
	RadioChangeToNextSource(emitterRef, type)
	ApplyFadeParams(emitterRef)
	CapturePersistentState(emitterRef)
EndFunction

int Function getMediaType()
	return RadioGetMediaType()
EndFunction

Function setStartupPlaylist(String playlist, ObjectReference targetEmitter = None)
        if playlist == ""
            StartupPlaylist = "Default"
        else
            StartupPlaylist = playlist
        endif

	ObjectReference emitterRef = targetEmitter
	if emitterRef == None
		emitterRef = ResolveEmitterForControls()
	endif
	if emitterRef == None
		return
	endif
	
	RadioPlayFx(emitterRef, "tuning_short")
	RadioChangePlaylist(emitterRef, StartupPlaylist)
	ApplyFadeParams(emitterRef)
	CapturePersistentState(emitterRef)

EndFunction


Function InitializeRadio()
	ObjectReference emitterRef = RadioEmitter

	if emitterRef == None
		emitterRef = Game.GetPlayer()
	endif
	if emitterRef == None
		return
	endif

	SetFallbackStationIndex(FallbackStationIndex)
	IsSFSEAvailable(emitterRef)
	RestorePersistentState(emitterRef)

	if AutoStartPlayback
		if UseStationStart
			RadioStart(emitterRef)
		else
			RadioPlay(emitterRef)
		endif
	endif

	SyncCellMusicMute()

	PushFadeSample()

	if UpdateFade
		; RegisterForSingleUpdate(FadeUpdateSeconds)
                StartTimer(FadeUpdateSeconds, MyTimer)

	endif
EndFunction

Event OnTimer(int aiTimerID)
        ; Debug.Notification("OnTimer().")
        Trace("OnTimer().")

        if aiTimerID == ControlTerminalTimer
            OpenControlTerminalNow()
            return
        endif

        if aiTimerID != MyTimer
            return  ; Ignore if wrong ID or stopped
        endif

	RefreshControlSlateAccess()
	SanitizeActiveEmitter()

	Actor player = Game.GetPlayer()
	ObjectReference controlsRef = ResolveEmitterForControls()
	if controlsRef != None
		CapturePersistentState(controlsRef)
	endif

		ObjectReference emitterRef = RadioEmitter
		if emitterRef == None
			emitterRef = controlsRef
		endif
		Trace("OnTimer state: controlsRef=" + controlsRef + " emitterRef=" + emitterRef + " carried=" + GetCarriedRadioCount())

			if emitterRef != None && RadioIsPlaying(emitterRef)
				if !IsTrackedEmitterStillReachable(emitterRef)
					; World radios pause when player is not in the same worldspace/cell.
					Trace("OnTimer: pausing unreachable emitter " + emitterRef)
					RadioPause(emitterRef)
					ResetTrackChangeNotification(emitterRef)
					; emitterRef = None
			elseif emitterRef != player
				; Keep fade updates only for in-world radios.
				Trace("OnTimer: push fade sample for emitter " + emitterRef)
				PushFadeSample(emitterRef)
				NotifyTrackChangeIfNeeded(emitterRef)
			else
				NotifyTrackChangeIfNeeded(emitterRef)
			endif
		else
			ResetTrackChangeNotification(emitterRef)
		endif

	UpdateDialogDuck(emitterRef)
	SyncCellMusicMute()

	if UpdateFade
		; RegisterForSingleUpdate(FadeUpdateSeconds)
                StartTimer(FadeUpdateSeconds, MyTimer)
	endif
EndEvent

Function UpdateDialogDuck(ObjectReference emitterRef)
	if emitterRef == None || !IsSFSEAvailable(emitterRef)
		return
	endif

	if !RadioIsPlaying(emitterRef)
		if InDialogDuck
			InDialogDuck = False
			PreDialogDuckVolume = -1.0
		endif
		return
	endif

	if !RadioSFSENative.getDialogDuckEnabled(emitterRef)
		if InDialogDuck
			if PreDialogDuckVolume >= 0.0
				RadioSetVolume(emitterRef, PreDialogDuckVolume)
			endif
			InDialogDuck = False
			PreDialogDuckVolume = -1.0
		endif
		return
	endif

	Bool inDialog = Game.IsPlayerInDialogue()
	if inDialog && !InDialogDuck
		Float duckVol = RadioSFSENative.getDialogDuckVolume(emitterRef)
		PreDialogDuckVolume = RadioGetVolume(emitterRef)
		RadioSetVolume(emitterRef, duckVol)
		InDialogDuck = True
		Trace("Dialog duck: " + PreDialogDuckVolume + " -> " + duckVol)
	elseif !inDialog && InDialogDuck
		if PreDialogDuckVolume >= 0.0
			RadioSetVolume(emitterRef, PreDialogDuckVolume)
			Trace("Dialog duck restored: " + PreDialogDuckVolume)
		endif
		InDialogDuck = False
		PreDialogDuckVolume = -1.0
	endif
EndFunction

Event OnQuestShutdown()
	SetMusicSilenceActive(False)
EndEvent

Function PushFadeSample(ObjectReference sampleEmitter = None)
	ObjectReference playerRef = Game.GetPlayer()
	if playerRef == None
		return
	endif

	ObjectReference emitterRef = sampleEmitter
	if emitterRef == None
		emitterRef = RadioEmitter
	endif
	if emitterRef == None
		emitterRef = playerRef
	endif

	if IsSFSEAvailable(emitterRef)
		RadioSFSENative.set_positions(emitterRef, emitterRef.GetPositionX(), emitterRef.GetPositionY(), emitterRef.GetPositionZ(), playerRef.GetPositionX(), playerRef.GetPositionY(), playerRef.GetPositionZ(), playerRef.GetAngleZ())
	endif
EndFunction

Alias Property PlayerAlias Auto Const
