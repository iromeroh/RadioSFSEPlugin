Scriptname STAR_Radio_Terminal_Menu_Script extends TerminalMenu Conditional

; Outpost terminal radio handler.
; CK setup:
; - Attach this script to the terminal menu form(s) used by the outpost terminal.
; - Set `myQuest` to STAR_Start_Quest.
; - Set `RadioTerminalMenu_Main` to the main menu form that contains the radio actions.
; - Optionally set `RadioTerminalMenu_Submenu` if actions are in a submenu.
; - Menu item IDs default to the same action mapping used by the portable radio UI:
;   0 media type, 1 source, 2 play/pause, 3 forward, 4 rewind, 5 volume up, 6 volume down, 7 play mode.
; Behavior:
; - Using the terminal promotes it to active emitter.
; - Optional stop of previous emitter prevents overlapping audio when swapping devices.
; - Reachability checks still rely on STAR_Start_Quest_Script timer/worldspace logic.

Quest Property myQuest Auto Mandatory ; -> STAR_Start_Quest
STAR_Start_Quest_Script Property mgr Auto

TerminalMenu Property RadioTerminalMenu_Main Auto Const Mandatory
TerminalMenu Property RadioTerminalMenu_Submenu Auto Const

Float Property VolumeStep = 20.0 Auto
Bool Property BypassOwnedRadioRequirement = True Auto
Bool Property StopPreviousEmitterOnActivate = True Auto
Bool Property UseControlsEmitter = False Auto

Int Property MenuItem_MediaType = 0 Auto Const
Int Property MenuItem_Source = 1 Auto Const
Int Property MenuItem_PlayPause = 2 Auto Const
Int Property MenuItem_Forward = 3 Auto Const
Int Property MenuItem_Rewind = 4 Auto Const
Int Property MenuItem_VolumeUp = 5 Auto Const
Int Property MenuItem_VolumeDown = 6 Auto Const
Int Property MenuItem_PlayMode = 7 Auto Const

Bool Function EnsureManager()
	if mgr == None
		mgr = myQuest as STAR_Start_Quest_Script
	endif
	return mgr != None
EndFunction

Function Notify(String text, Bool force = False)
	if text == ""
		return
	endif
	if EnsureManager()
		mgr.Notify(text, force)
	elseif force
		Debug.Notification(text)
	endif
EndFunction

Function Trace(String text)
	if EnsureManager()
		mgr.Trace("STAR_Radio_Terminal_Menu_Script: " + text)
	endif
EndFunction

Bool Function IsHandledMenu(TerminalMenu akTerminalBase)
	if akTerminalBase == RadioTerminalMenu_Main
		return True
	endif
	if RadioTerminalMenu_Submenu != None && akTerminalBase == RadioTerminalMenu_Submenu
		return True
	endif
	return False
EndFunction

Bool Function WaitForPlaybackResult(ObjectReference emitterRef, Float timeoutSeconds = 6.0, Float stepSeconds = 0.25)
	Float elapsed = 0.0
	while elapsed < timeoutSeconds
		if mgr.RadioIsPlaying(emitterRef)
			return true
		endif

		String errTick = mgr.RadioLastError(emitterRef)
		if errTick != ""
			return false
		endif

		Utility.Wait(stepSeconds)
		elapsed += stepSeconds
	endwhile

	return mgr.RadioIsPlaying(emitterRef)
EndFunction

Bool Function PlayFxIfReachable(ObjectReference emitterRef, String fxBasename)
	if emitterRef == None || fxBasename == ""
		return false
	endif
	if !mgr.IsEmitterReachableFromPlayer(emitterRef)
		return false
	endif
	return mgr.RadioPlayFx(emitterRef, fxBasename)
EndFunction

ObjectReference Function ResolveMenuEmitter(ObjectReference akTerminalRef)
	if !UseControlsEmitter
		return akTerminalRef
	endif

	return mgr.ResolveEmitterForControls()
EndFunction

Function PromoteTerminalAsActiveEmitter(ObjectReference akTerminalRef)
	if !EnsureManager() || akTerminalRef == None
		return
	endif

	; Register this ref as a fixed-tuner device before any other native call so that
	; all subsequent calls on akTerminalRef are routed to the shared fixed device slot.
	RadioSFSENative.notifyDeviceClass(akTerminalRef, 1)

	ObjectReference previousEmitter = mgr.getEmitter()
	if previousEmitter == akTerminalRef
		mgr.UpdateLastRadioLocation(akTerminalRef)
		return
	endif

	mgr.DeactivateHeadsetForExternalRadio()

	; Read the fixed slot's current native source now, before saving/stopping the previous
	; emitter, so we can decide below whether to initialise from defaults or just restore.
	String fixedNativeSource = mgr.RadioCurrentSourceName(akTerminalRef)

	if previousEmitter != None
		mgr.CapturePersistentState(previousEmitter)
		if StopPreviousEmitterOnActivate && mgr.RadioIsPlaying(previousEmitter)
			mgr.RadioStop(previousEmitter)
		endif
	endif

	mgr.setEmitter(akTerminalRef)
	mgr.UpdateLastRadioLocation(akTerminalRef)

	if fixedNativeSource != ""
		; Fixed slot has its own persisted state — just sync fade params.
		mgr.ApplyFadeParams(akTerminalRef)
	else
		; Fixed slot is fresh (first ever use or JSON was cleared).
		; Initialise from the startup playlist rather than inheriting the portable's state.
		Int startMediaType = mgr.getMediaType()
		mgr.RadioChangeToNextSource(akTerminalRef, startMediaType)
		String startSource = mgr.RadioCurrentSourceName(akTerminalRef)
		if startSource == "" && mgr.StartupPlaylist != ""
			mgr.RadioChangePlaylist(akTerminalRef, mgr.StartupPlaylist)
		endif
		mgr.ApplyFadeParams(akTerminalRef)
	endif

	mgr.CapturePersistentState(akTerminalRef)
	mgr.SyncCellMusicMute(akTerminalRef)
EndFunction

Event OnTerminalMenuEnter(TerminalMenu akTerminalBase, ObjectReference akTerminalRef)
	if !IsHandledMenu(akTerminalBase)
		return
	endif

	if akTerminalRef == None
		return
	endif

	if !EnsureManager()
		Notify("Quest script missing.", true)
		return
	endif

	if !UseControlsEmitter
		PromoteTerminalAsActiveEmitter(akTerminalRef)
	endif
EndEvent

Event OnTerminalMenuItemRun(int auiMenuItemID, TerminalMenu akTerminalBase, ObjectReference akTerminalRef)
	if !IsHandledMenu(akTerminalBase)
		return
	endif

	if akTerminalRef == None
		return
	endif

	if !EnsureManager()
		Notify("Quest script missing.", true)
		return
	endif

	if !BypassOwnedRadioRequirement && !mgr.canUseRadioControls()
		Notify("Build or buy a radio first.", true)
		return
	endif

	if auiMenuItemID == 65535
		; Terminal menu close/cancel sentinel.
		return
	endif

	ObjectReference emitterRef = ResolveMenuEmitter(akTerminalRef)
	if emitterRef == None
		Notify("No radio device.", true)
		return
	endif

	if !UseControlsEmitter
		PromoteTerminalAsActiveEmitter(akTerminalRef)
		emitterRef = akTerminalRef
	endif

	if !mgr.IsEmitterReachableFromPlayer(emitterRef)
		if mgr.RadioIsPlaying(emitterRef)
			mgr.RadioPause(emitterRef)
		endif
		if UseControlsEmitter
			Notify("Radio is not here.", true)
		else
			Notify("Terminal radio is not reachable.", true)
		endif
		return
	endif

	if auiMenuItemID == MenuItem_MediaType
		Int mediaType = mgr.getMediaType()
		if mediaType >= 3
			mediaType = 1
		else
			mediaType = mediaType + 1
		endif

		mgr.setMediaType(mediaType, emitterRef)

		if mediaType == 2 || mediaType == 3
			PlayFxIfReachable(emitterRef, "tuning_short.mp3")
		else
			PlayFxIfReachable(emitterRef, "notification.mp3")
		endif

		String sourceName = mgr.RadioCurrentSourceName(emitterRef)
		if sourceName == ""
			if mgr.RadioGetMediaType(emitterRef) == mediaType
				Notify("Media type " + mediaType + " selected. No sources found.", true)
			else
				String err0 = mgr.RadioLastError(emitterRef)
				if err0 == ""
					err0 = "Could not change media type."
				endif
				Notify(err0, true)
			endif
		else
			Notify("Media type " + mediaType + ": " + sourceName, true)
		endif
		mgr.CapturePersistentState(emitterRef)

	elseif auiMenuItemID == MenuItem_Source
		Int mediaType2 = mgr.getMediaType()
		if mediaType2 == 2 || mediaType2 == 3
			PlayFxIfReachable(emitterRef, "tuning_short.mp3")
		else
			PlayFxIfReachable(emitterRef, "notification.mp3")
		endif

		Bool selected = mgr.RadioSelectNextSource(emitterRef, mediaType2)
		if !selected
			String err1 = mgr.RadioLastError(emitterRef)
			if err1 == ""
				err1 = "Could not change playlist/source."
			endif
			Notify(err1, true)
		else
			String sourceName2 = mgr.RadioCurrentSourceName(emitterRef)
			Notify("Selected: " + sourceName2, true)
		endif
		mgr.CapturePersistentState(emitterRef)

	elseif auiMenuItemID == MenuItem_PlayPause
		if mgr.RadioIsPlaying(emitterRef)
			mgr.RadioPause(emitterRef)
			String err2p = mgr.RadioLastError(emitterRef)
			if err2p != ""
				Notify(err2p, true)
			else
				Notify("Playback paused.")
			endif
		else
			if mgr.getMediaType() == 3
				PlayFxIfReachable(emitterRef, "tuning_long.mp3")
			endif
			mgr.RadioPlay(emitterRef)
			if WaitForPlaybackResult(emitterRef)
				String nowName = mgr.RadioCurrentTrackBasename(emitterRef)
				if nowName == ""
					nowName = mgr.RadioCurrentSourceName(emitterRef)
				endif
				Notify("Now playing: " + nowName, true)
			else
				String err2 = mgr.RadioLastError(emitterRef)
				if err2 == ""
					err2 = "Could not start playback."
				endif
				PlayFxIfReachable(emitterRef, "no_station.mp3")
				Notify(err2, true)
			endif
		endif
		mgr.CapturePersistentState(emitterRef)

	elseif auiMenuItemID == MenuItem_Forward
		mgr.RadioForward(emitterRef)
		String err3 = mgr.RadioLastError(emitterRef)
		if err3 != "" && !mgr.RadioIsPlaying(emitterRef)
			Utility.Wait(0.3)
			mgr.RadioForward(emitterRef)
			err3 = mgr.RadioLastError(emitterRef)
		endif

		if err3 != ""
			Notify(err3, true)
		else
			String nextName = mgr.RadioCurrentTrackBasename(emitterRef)
			if nextName == ""
				nextName = mgr.RadioCurrentSourceName(emitterRef)
			endif
			Notify("Now playing: " + nextName, true)
		endif
		mgr.CapturePersistentState(emitterRef)

	elseif auiMenuItemID == MenuItem_Rewind
		mgr.RadioRewind(emitterRef)
		String errRewind = mgr.RadioLastError(emitterRef)
		if errRewind != "" && !mgr.RadioIsPlaying(emitterRef)
			Utility.Wait(0.3)
			mgr.RadioRewind(emitterRef)
			errRewind = mgr.RadioLastError(emitterRef)
		endif

		if errRewind != ""
			Notify(errRewind, true)
		else
			String rewindName = mgr.RadioCurrentTrackBasename(emitterRef)
			if rewindName == ""
				rewindName = mgr.RadioCurrentSourceName(emitterRef)
			endif
			Notify("Now playing: " + rewindName, true)
		endif
		mgr.CapturePersistentState(emitterRef)

	elseif auiMenuItemID == MenuItem_VolumeUp
		Float volumeStepNow = mgr.ResolveVolumeStepPercent(emitterRef, VolumeStep)
		Bool volUpOk = mgr.RadioVolumeUp(emitterRef, volumeStepNow)
		if !volUpOk
			String err4 = mgr.RadioLastError(emitterRef)
			if err4 == ""
				err4 = "Could not increase volume."
			endif
			Notify(err4, true)
		else
			Float volUp = mgr.RadioGetVolume(emitterRef)
			Notify("Volume: " + volUp, true)
		endif
		mgr.CapturePersistentState(emitterRef)

	elseif auiMenuItemID == MenuItem_VolumeDown
		Float volumeStepNowDown = mgr.ResolveVolumeStepPercent(emitterRef, VolumeStep)
		Bool volDownOk = mgr.RadioVolumeDown(emitterRef, volumeStepNowDown)
		if !volDownOk
			String err5 = mgr.RadioLastError(emitterRef)
			if err5 == ""
				err5 = "Could not decrease volume."
			endif
			Notify(err5, true)
		else
			Float volDown = mgr.RadioGetVolume(emitterRef)
			Notify("Volume: " + volDown, true)
		endif
		mgr.CapturePersistentState(emitterRef)

	elseif auiMenuItemID == MenuItem_PlayMode
		Int playMode = mgr.RadioGetPlayMode(emitterRef)
		if playMode == 2
			playMode = 1
		else
			playMode = 2
		endif

		Bool playModeOk = mgr.RadioSetPlayMode(emitterRef, playMode)
		if !playModeOk
			String errPlayMode = mgr.RadioLastError(emitterRef)
			if errPlayMode == ""
				errPlayMode = "Could not change play mode."
			endif
			Notify(errPlayMode, true)
		else
			Notify("Play mode: " + mgr.PlayModeName(playMode), true)
		endif
		mgr.CapturePersistentState(emitterRef)
	else
		Trace("Unhandled menu item id: " + auiMenuItemID + " base=" + akTerminalBase)
	endif

	mgr.SyncCellMusicMute(emitterRef)
EndEvent
