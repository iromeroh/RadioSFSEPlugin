Scriptname STAR_Start_Quest_Script extends Quest

ObjectReference Property permanentRadioEmitter Auto Mandatory
ObjectReference Property RadioEmitter Auto
String Property StartupPlaylist = "Default" Auto

Book Property playStopSlate Auto  ; Drag your slate form here in CK
;Book Property forwardSlate Auto  ; Drag your slate form here in CK
;Book Property mediaTypeSlate Auto  ; Drag your slate form here in CK
;Book Property playlistSlate Auto  ; Drag your slate form here in CK

; Vendor merchant containers (references) to seed with radios on first quest init.
; New Atlantis
ObjectReference Property VendorChest_JemisonMercantile Auto
ObjectReference Property VendorChest_UCDistribution Auto
; Cydonia
ObjectReference Property VendorChest_CydoniaTradeAuthority Auto
ObjectReference Property VendorChest_CydoniaElectronics Auto
; Akila
ObjectReference Property VendorChest_AkilaGeneralStore Auto
ObjectReference Property VendorChest_AkilaTradeAuthority Auto
; Neon
ObjectReference Property VendorChest_NeonSiegharts Auto
ObjectReference Property VendorChest_NeonTradeAuthority Auto

Bool Property AutoStartPlayback = False Auto
Bool Property UseStationStart = False Auto
Bool Property UpdateFade = True Auto
Float Property FadeUpdateSeconds = 0.25 Auto
WorldSpace Property lastRadioWorldspace = None Auto
Cell Property lastRadioCell = None Auto Hidden
Bool Property RequireOwnedRadioForControls = True Auto
Bool Property GiveStarterRadioOnInit = False Auto
Bool Property RadioControlsUnlocked = False Auto Hidden
Bool Property VerboseNotifications = False Auto
Bool Property VerboseTraceLogs = False Auto
Int Property VendorStockMin = 2 Auto
Int Property VendorStockMax = 3 Auto
Bool Property VendorStockSeeded = False Auto Hidden
Bool Property HasPersistentState = False Auto Hidden
String Property PersistentSourceName = "" Auto Hidden
String Property PersistentTrackName = "" Auto Hidden
Float Property PersistentVolume = 60.0 Auto Hidden
Bool Property NotifyTrackChangesOnTimer = True Auto
ObjectReference Property LastTrackNotifyEmitter = None Auto Hidden
String Property LastTrackNotifiedName = "" Auto Hidden
Bool Property UseCustomFadeParams = False Auto
Float Property FadeMinDistance = 0.1 Auto
Float Property FadeMaxDistance = 35.0 Auto
Float Property FadePanDistance = 5.0 Auto
Bool Property AutoDisableLegacyFadeOverride = True Auto
Bool Property MuteCellMusicWhileRadioPlaying = True Auto
MusicType Property MusicSilenceOverride Auto
Bool Property MusicSilenceActive = False Auto Hidden

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

Bool Property SFSEProbeDone = False Auto Hidden
Bool Property SFSEAvailable = False Auto Hidden
Bool Property SFSEMissingNoticeShown = False Auto Hidden
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

int MyTimer = 528
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
		Int category = mediaType
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
	if VerboseTraceLogs
		return
	endif

	VerboseTraceLogs = True
	Debug.Trace("STAR_Radio: VerboseTraceLogs enabled for diagnostics.")
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
	EnsureDiagnosticsEnabled()
	RefreshControlSlateAccess()
	SeedVendorStock()

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
	SetFallbackStationIndex(FallbackStationIndex)
EndFunction

Form Function RadioBaseForm()
	if permanentRadioEmitter == None
		return None
	endif
	return permanentRadioEmitter.getBaseObject()
EndFunction

int Function GetCarriedRadioCount()
	Actor player = Game.GetPlayer()
	Form radioBase = RadioBaseForm()
	if player == None || radioBase == None
		return 0
	endif
	return player.GetItemCount(radioBase)
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
		return GetCarriedRadioCount() > 0
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
		int carried = GetCarriedRadioCount()
		Bool reachableOnPlayer = carried > 0
		if !reachableOnPlayer
			Trace("IsEmitterReachableFromPlayer: false (emitter=player, carried=0).")
		endif
		return reachableOnPlayer
	endif

	Bool reachable = IsReferenceReachableFromPlayer(emitterRef)
	if !reachable
		Trace("IsEmitterReachableFromPlayer: false (world/cell mismatch) emitter=" + emitterRef)
	endif
	return reachable
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
	if mediaType != 1 && mediaType != 2
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
		return
	endif

	if playStopSlate != None
		int c0 = player.GetItemCount(playStopSlate)
		if c0 > 0
			player.RemoveItem(playStopSlate, c0, true)
		endif
	endif
EndFunction

Function SeedVendorStock()
	if VendorStockSeeded
		return
	endif

	Form radioBase = RadioBaseForm()
	if radioBase == None
		Trace("SeedVendorStock skipped: radio base form is missing.")
		return
	endif

	AddRadioStockToChest(VendorChest_JemisonMercantile, radioBase, "Jemison Mercantile")
	AddRadioStockToChest(VendorChest_UCDistribution, radioBase, "UC Distribution")
	AddRadioStockToChest(VendorChest_CydoniaTradeAuthority, radioBase, "Cydonia Trade Authority")
	AddRadioStockToChest(VendorChest_CydoniaElectronics, radioBase, "Cydonia Electronics")
	AddRadioStockToChest(VendorChest_AkilaGeneralStore, radioBase, "Akila General Store")
	AddRadioStockToChest(VendorChest_AkilaTradeAuthority, radioBase, "Akila Trade Authority")
	AddRadioStockToChest(VendorChest_NeonSiegharts, radioBase, "Neon Sieghart's")
	AddRadioStockToChest(VendorChest_NeonTradeAuthority, radioBase, "Neon Trade Authority")

	VendorStockSeeded = True
EndFunction

Function AddRadioStockToChest(ObjectReference chestRef, Form radioBase, String label)
	if chestRef == None || radioBase == None
		return
	endif

	int minCount = VendorStockMin
	if minCount < 1
		minCount = 1
	endif

	int maxCount = VendorStockMax
	if maxCount < minCount
		maxCount = minCount
	endif

	int targetCount = Utility.RandomInt(minCount, maxCount)
	int currentCount = chestRef.GetItemCount(radioBase)
	int addCount = targetCount - currentCount

	if addCount > 0
		chestRef.AddItem(radioBase, addCount, true)
		Trace("Seeded " + label + " with +" + addCount + " radios.")
	else
		Trace("Seed skipped for " + label + ". Existing radios: " + currentCount)
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

	if GetCarriedRadioCount() > 0
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

	int carriedRadios = GetCarriedRadioCount()
	ObjectReference mainEmitter = RadioEmitter

	if mainEmitter != None && mainEmitter != player
		if IsReferenceReachableFromPlayer(mainEmitter)
			return mainEmitter
		endif

		if carriedRadios > 0
			return player
		endif

		return None
	endif

	if carriedRadios > 0
		return player
	endif

	return None
EndFunction

ObjectReference Function ResolveEmitterForState()
	ObjectReference emitterRef = getEmitter()
	if emitterRef == None
		emitterRef = permanentRadioEmitter
	endif
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
	if emitterRef == None
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
	if emitterRef == None
		emitterRef = ResolveEmitterForState()
	endif
	if emitterRef == None
		return
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
	return mediaType
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

        if ! emitterRef
            emitterRef = permanentRadioEmitter
        endif
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
        EnsureDiagnosticsEnabled()
        Trace("OnTimer().")

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
			if !IsEmitterReachableFromPlayer(emitterRef)
				; World radios pause when player is not in the same worldspace/cell.
				Trace("OnTimer: pausing unreachable emitter " + emitterRef)
				RadioPause(emitterRef)
				ResetTrackChangeNotification(emitterRef)
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

	SyncCellMusicMute()

	if UpdateFade
		; RegisterForSingleUpdate(FadeUpdateSeconds)
                StartTimer(FadeUpdateSeconds, MyTimer)
	endif
EndEvent

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
