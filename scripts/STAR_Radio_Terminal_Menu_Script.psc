Scriptname STAR_Radio_Terminal_Menu_Script extends TerminalMenu Conditional

; Outpost terminal radio handler.
; CK setup:
; - Attach this script to the terminal menu form(s) used by the outpost terminal.
; - Set `myQuest` to STAR_Start_Quest.
; - Set `RadioTerminalMenu_Main` to the main menu form that contains the radio actions.
; - Optionally set `RadioTerminalMenu_Submenu` if actions are in a submenu.
; - Menu item IDs default to the same action mapping used by the portable radio UI:
;   0 media type, 1 source, 2 play/pause, 3 forward, 4 rewind, 5 volume up, 6 volume down.
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

Int Property MenuItem_MediaType = 0 Auto Const
Int Property MenuItem_Source = 1 Auto Const
Int Property MenuItem_PlayPause = 2 Auto Const
Int Property MenuItem_Forward = 3 Auto Const
Int Property MenuItem_Rewind = 4 Auto Const
Int Property MenuItem_VolumeUp = 5 Auto Const
Int Property MenuItem_VolumeDown = 6 Auto Const

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

Function PromoteTerminalAsActiveEmitter(ObjectReference akTerminalRef)
	if !EnsureManager() || akTerminalRef == None
		return
	endif

	ObjectReference previousEmitter = mgr.getEmitter()
	if previousEmitter == akTerminalRef
		mgr.UpdateLastRadioLocation(akTerminalRef)
		return
	endif

	if previousEmitter != None
		mgr.CapturePersistentState(previousEmitter)
		if StopPreviousEmitterOnActivate && mgr.RadioIsPlaying(previousEmitter)
			mgr.RadioStop(previousEmitter)
		endif
	endif

	mgr.setEmitter(akTerminalRef)
	mgr.UpdateLastRadioLocation(akTerminalRef)
	mgr.ApplyPersistentStateToEmitter(akTerminalRef)
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

	PromoteTerminalAsActiveEmitter(akTerminalRef)
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

	PromoteTerminalAsActiveEmitter(akTerminalRef)
	if !mgr.IsEmitterReachableFromPlayer(akTerminalRef)
		if mgr.RadioIsPlaying(akTerminalRef)
			mgr.RadioPause(akTerminalRef)
		endif
		Notify("Terminal radio is not reachable.", true)
		return
	endif

	if auiMenuItemID == MenuItem_MediaType
		Int mediaType = mgr.getMediaType()
		if mediaType >= 3
			mediaType = 1
		else
			mediaType = mediaType + 1
		endif

		mgr.setMediaType(mediaType, akTerminalRef)

		if mediaType == 2 || mediaType == 3
			PlayFxIfReachable(akTerminalRef, "tuning_short.mp3")
		else
			PlayFxIfReachable(akTerminalRef, "notification.mp3")
		endif

		String sourceName = mgr.RadioCurrentSourceName(akTerminalRef)
		if sourceName == ""
			String err0 = mgr.RadioLastError(akTerminalRef)
			if err0 == ""
				err0 = "Could not change media type."
			endif
			Notify(err0, true)
		else
			Notify("Media type " + mediaType + ": " + sourceName, true)
		endif
		mgr.CapturePersistentState(akTerminalRef)

	elseif auiMenuItemID == MenuItem_Source
		Int mediaType2 = mgr.getMediaType()
		if mediaType2 == 2 || mediaType2 == 3
			PlayFxIfReachable(akTerminalRef, "tuning_short.mp3")
		else
			PlayFxIfReachable(akTerminalRef, "notification.mp3")
		endif

		Bool selected = mgr.RadioSelectNextSource(akTerminalRef, mediaType2)
		if !selected
			String err1 = mgr.RadioLastError(akTerminalRef)
			if err1 == ""
				err1 = "Could not change playlist/source."
			endif
			Notify(err1, true)
		else
			String sourceName2 = mgr.RadioCurrentSourceName(akTerminalRef)
			Notify("Selected: " + sourceName2, true)
		endif
		mgr.CapturePersistentState(akTerminalRef)

	elseif auiMenuItemID == MenuItem_PlayPause
		if mgr.RadioIsPlaying(akTerminalRef)
			mgr.RadioPause(akTerminalRef)
			String err2p = mgr.RadioLastError(akTerminalRef)
			if err2p != ""
				Notify(err2p, true)
			else
				Notify("Playback paused.")
			endif
		else
			if mgr.getMediaType() == 3
				PlayFxIfReachable(akTerminalRef, "tuning_long.mp3")
			endif
			mgr.RadioPlay(akTerminalRef)
			if WaitForPlaybackResult(akTerminalRef)
				String nowName = mgr.RadioCurrentTrackBasename(akTerminalRef)
				if nowName == ""
					nowName = mgr.RadioCurrentSourceName(akTerminalRef)
				endif
				Notify("Now playing: " + nowName, true)
			else
				String err2 = mgr.RadioLastError(akTerminalRef)
				if err2 == ""
					err2 = "Could not start playback."
				endif
				PlayFxIfReachable(akTerminalRef, "no_station.mp3")
				Notify(err2, true)
			endif
		endif
		mgr.CapturePersistentState(akTerminalRef)

	elseif auiMenuItemID == MenuItem_Forward
		mgr.RadioForward(akTerminalRef)
		String err3 = mgr.RadioLastError(akTerminalRef)
		if err3 != "" && !mgr.RadioIsPlaying(akTerminalRef)
			Utility.Wait(0.3)
			mgr.RadioForward(akTerminalRef)
			err3 = mgr.RadioLastError(akTerminalRef)
		endif

		if err3 != ""
			Notify(err3, true)
		else
			String nextName = mgr.RadioCurrentTrackBasename(akTerminalRef)
			if nextName == ""
				nextName = mgr.RadioCurrentSourceName(akTerminalRef)
			endif
			Notify("Now playing: " + nextName, true)
		endif
		mgr.CapturePersistentState(akTerminalRef)

	elseif auiMenuItemID == MenuItem_Rewind
		mgr.RadioRewind(akTerminalRef)
		String errRewind = mgr.RadioLastError(akTerminalRef)
		if errRewind != "" && !mgr.RadioIsPlaying(akTerminalRef)
			Utility.Wait(0.3)
			mgr.RadioRewind(akTerminalRef)
			errRewind = mgr.RadioLastError(akTerminalRef)
		endif

		if errRewind != ""
			Notify(errRewind, true)
		else
			String rewindName = mgr.RadioCurrentTrackBasename(akTerminalRef)
			if rewindName == ""
				rewindName = mgr.RadioCurrentSourceName(akTerminalRef)
			endif
			Notify("Now playing: " + rewindName, true)
		endif
		mgr.CapturePersistentState(akTerminalRef)

	elseif auiMenuItemID == MenuItem_VolumeUp
		Float volumeStepNow = mgr.ResolveVolumeStepPercent(akTerminalRef, VolumeStep)
		Bool volUpOk = mgr.RadioVolumeUp(akTerminalRef, volumeStepNow)
		if !volUpOk
			String err4 = mgr.RadioLastError(akTerminalRef)
			if err4 == ""
				err4 = "Could not increase volume."
			endif
			Notify(err4, true)
		else
			Float volUp = mgr.RadioGetVolume(akTerminalRef)
			Notify("Volume: " + volUp, true)
		endif
		mgr.CapturePersistentState(akTerminalRef)

	elseif auiMenuItemID == MenuItem_VolumeDown
		Float volumeStepNowDown = mgr.ResolveVolumeStepPercent(akTerminalRef, VolumeStep)
		Bool volDownOk = mgr.RadioVolumeDown(akTerminalRef, volumeStepNowDown)
		if !volDownOk
			String err5 = mgr.RadioLastError(akTerminalRef)
			if err5 == ""
				err5 = "Could not decrease volume."
			endif
			Notify(err5, true)
		else
			Float volDown = mgr.RadioGetVolume(akTerminalRef)
			Notify("Volume: " + volDown, true)
		endif
		mgr.CapturePersistentState(akTerminalRef)
	else
		Trace("Unhandled menu item id: " + auiMenuItemID + " base=" + akTerminalBase)
	endif

	mgr.SyncCellMusicMute(akTerminalRef)
EndEvent
