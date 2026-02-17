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
		if RadioSFSENative.isPlaying(emitterRef)
			return true
		endif

		String errTick = RadioSFSENative.lastError(emitterRef)
		if errTick != ""
			return false
		endif

		Utility.Wait(stepSeconds)
		elapsed += stepSeconds
	endwhile

	return RadioSFSENative.isPlaying(emitterRef)
EndFunction

Bool Function PlayFxIfReachable(ObjectReference emitterRef, String fxBasename)
	if emitterRef == None || fxBasename == ""
		return false
	endif
	if !mgr.IsEmitterReachableFromPlayer(emitterRef)
		return false
	endif
	return RadioSFSENative.playFx(emitterRef, fxBasename)
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
		if StopPreviousEmitterOnActivate && RadioSFSENative.isPlaying(previousEmitter)
			RadioSFSENative.stop(previousEmitter)
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

	PromoteTerminalAsActiveEmitter(akTerminalRef)
	if !mgr.IsEmitterReachableFromPlayer(akTerminalRef)
		if RadioSFSENative.isPlaying(akTerminalRef)
			RadioSFSENative.pause(akTerminalRef)
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

		String sourceName = RadioSFSENative.currentSourceName(akTerminalRef)
		if sourceName == ""
			String err0 = RadioSFSENative.lastError(akTerminalRef)
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

		Bool selected = RadioSFSENative.selectNextSource(akTerminalRef, mediaType2)
		if !selected
			String err1 = RadioSFSENative.lastError(akTerminalRef)
			if err1 == ""
				err1 = "Could not change playlist/source."
			endif
			Notify(err1, true)
		else
			String sourceName2 = RadioSFSENative.currentSourceName(akTerminalRef)
			Notify("Selected: " + sourceName2, true)
		endif
		mgr.CapturePersistentState(akTerminalRef)

	elseif auiMenuItemID == MenuItem_PlayPause
		if RadioSFSENative.isPlaying(akTerminalRef)
			RadioSFSENative.pause(akTerminalRef)
			String err2p = RadioSFSENative.lastError(akTerminalRef)
			if err2p != ""
				Notify(err2p, true)
			else
				Notify("Playback paused.")
			endif
		else
			if mgr.getMediaType() == 3
				PlayFxIfReachable(akTerminalRef, "tuning_long.mp3")
			endif
			RadioSFSENative.play(akTerminalRef)
			if WaitForPlaybackResult(akTerminalRef)
				String nowName = RadioSFSENative.currentTrackBasename(akTerminalRef)
				if nowName == ""
					nowName = RadioSFSENative.currentSourceName(akTerminalRef)
				endif
				Notify("Now playing: " + nowName, true)
			else
				String err2 = RadioSFSENative.lastError(akTerminalRef)
				if err2 == ""
					err2 = "Could not start playback."
				endif
				PlayFxIfReachable(akTerminalRef, "no_station.mp3")
				Notify(err2, true)
			endif
		endif
		mgr.CapturePersistentState(akTerminalRef)

	elseif auiMenuItemID == MenuItem_Forward
		RadioSFSENative.forward(akTerminalRef)
		String err3 = RadioSFSENative.lastError(akTerminalRef)
		if err3 != "" && !RadioSFSENative.isPlaying(akTerminalRef)
			Utility.Wait(0.3)
			RadioSFSENative.forward(akTerminalRef)
			err3 = RadioSFSENative.lastError(akTerminalRef)
		endif

		if err3 != ""
			Notify(err3, true)
		else
			String nextName = RadioSFSENative.currentTrackBasename(akTerminalRef)
			if nextName == ""
				nextName = RadioSFSENative.currentSourceName(akTerminalRef)
			endif
			Notify("Now playing: " + nextName, true)
		endif
		mgr.CapturePersistentState(akTerminalRef)

	elseif auiMenuItemID == MenuItem_Rewind
		RadioSFSENative.rewind(akTerminalRef)
		String errRewind = RadioSFSENative.lastError(akTerminalRef)
		if errRewind != "" && !RadioSFSENative.isPlaying(akTerminalRef)
			Utility.Wait(0.3)
			RadioSFSENative.rewind(akTerminalRef)
			errRewind = RadioSFSENative.lastError(akTerminalRef)
		endif

		if errRewind != ""
			Notify(errRewind, true)
		else
			String rewindName = RadioSFSENative.currentTrackBasename(akTerminalRef)
			if rewindName == ""
				rewindName = RadioSFSENative.currentSourceName(akTerminalRef)
			endif
			Notify("Now playing: " + rewindName, true)
		endif
		mgr.CapturePersistentState(akTerminalRef)

	elseif auiMenuItemID == MenuItem_VolumeUp
		Bool volUpOk = RadioSFSENative.volumeUp(akTerminalRef, VolumeStep)
		if !volUpOk
			String err4 = RadioSFSENative.lastError(akTerminalRef)
			if err4 == ""
				err4 = "Could not increase volume."
			endif
			Notify(err4, true)
		else
			Float volUp = RadioSFSENative.getVolume(akTerminalRef)
			Notify("Volume: " + volUp, true)
		endif
		mgr.CapturePersistentState(akTerminalRef)

	elseif auiMenuItemID == MenuItem_VolumeDown
		Bool volDownOk = RadioSFSENative.volumeDown(akTerminalRef, VolumeStep)
		if !volDownOk
			String err5 = RadioSFSENative.lastError(akTerminalRef)
			if err5 == ""
				err5 = "Could not decrease volume."
			endif
			Notify(err5, true)
		else
			Float volDown = RadioSFSENative.getVolume(akTerminalRef)
			Notify("Volume: " + volDown, true)
		endif
		mgr.CapturePersistentState(akTerminalRef)
	else
		Trace("Unhandled menu item id: " + auiMenuItemID + " base=" + akTerminalBase)
	endif

	mgr.SyncCellMusicMute(akTerminalRef)
EndEvent
