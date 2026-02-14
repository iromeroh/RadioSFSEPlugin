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
Float Property FadeUpdateSeconds = 0.5 Auto
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
Bool Property UseCustomFadeParams = False Auto
Float Property FadeMinDistance = 0.1 Auto
Float Property FadeMaxDistance = 35.0 Auto
Float Property FadePanDistance = 40.0 Auto
Bool Property AutoDisableLegacyFadeOverride = True Auto

int MyTimer = 528
int mediaType = 1

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
	        EnsureDiagnosticsEnabled()
	        RefreshControlSlateAccess()
		SeedVendorStock()
	        
		InitializeRadio()
EndEvent

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
		RadioSFSENative.setFadeParams(emitterRef, -1.0, -1.0, -1.0)
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

	RadioSFSENative.setFadeParams(emitterRef, minDist, maxDist, panDist)
	Trace("ApplyFadeParams: custom min=" + minDist + " max=" + maxDist + " pan=" + panDist + " emitter=" + emitterRef)
EndFunction

Function ApplyPersistentStateToEmitter(ObjectReference emitterRef)
	if emitterRef == None
		return
	endif

	RadioSFSENative.changeToNextSource(emitterRef, mediaType)

	String sourceToApply = PersistentSourceName
	if sourceToApply == ""
		sourceToApply = StartupPlaylist
	endif
	if sourceToApply != ""
		RadioSFSENative.change_playlist(emitterRef, sourceToApply)
	endif

	if PersistentTrackName != "" && PersistentTrackName != "na"
		RadioSFSENative.setTrack(emitterRef, PersistentTrackName)
	endif

	RadioSFSENative.setVolume(emitterRef, PersistentVolume)
	ApplyFadeParams(emitterRef)
EndFunction

Function CapturePersistentState(ObjectReference emitterRef = None)
	if emitterRef == None
		emitterRef = ResolveEmitterForState()
	endif
	if emitterRef == None
		return
	endif

	String sourceName = RadioSFSENative.currentSourceName(emitterRef)
	if sourceName != ""
		PersistentSourceName = sourceName
		HasPersistentState = True
	endif

	String trackName = RadioSFSENative.getTrack(emitterRef)
	if trackName != ""
		PersistentTrackName = trackName
	endif

	Float volumeNow = RadioSFSENative.getVolume(emitterRef)
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
	RadioSFSENative.playFx(emitterRef, "tuning_short")
	RadioSFSENative.changeToNextSource(emitterRef, type)
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
	
	RadioSFSENative.playFx(emitterRef, "tuning_short")
	RadioSFSENative.change_playlist(emitterRef, StartupPlaylist)
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

	RestorePersistentState(emitterRef)

	if AutoStartPlayback
		if UseStationStart
			RadioSFSENative.start(emitterRef)
		else
			RadioSFSENative.play(emitterRef)
		endif
	endif

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

		if emitterRef != None && RadioSFSENative.isPlaying(emitterRef)
			if !IsEmitterReachableFromPlayer(emitterRef)
				; World radios pause when player is not in the same worldspace/cell.
				Trace("OnTimer: pausing unreachable emitter " + emitterRef)
				RadioSFSENative.pause(emitterRef)
			elseif emitterRef != player
				; Keep fade updates only for in-world radios.
				Trace("OnTimer: push fade sample for emitter " + emitterRef)
				PushFadeSample(emitterRef)
			endif
		endif

	if UpdateFade
		; RegisterForSingleUpdate(FadeUpdateSeconds)
                StartTimer(FadeUpdateSeconds, MyTimer)
	endif
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

	RadioSFSENative.set_positions( emitterRef,emitterRef.GetPositionX(),emitterRef.GetPositionY(),emitterRef.GetPositionZ(),playerRef.GetPositionX(),playerRef.GetPositionY(),	playerRef.GetPositionZ(), playerRef.GetAngleZ())
EndFunction

Alias Property PlayerAlias Auto Const
