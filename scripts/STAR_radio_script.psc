Scriptname STAR_radio_script extends ObjectReference

Quest Property myQuest Auto Mandatory ; -> STAR_Start_Quest
STAR_Start_Quest_Script Property mgr Auto

; Define the message box/menu items
Message Property MyMenuMessage Auto
Float Property VolumeStep = 20.0 Auto


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
		mgr.Trace("STAR_radio_script: " + text)
	endif
EndFunction

Bool Function IsRadioReachableForFx(ObjectReference emitterRef)
	if !EnsureManager()
		return false
	endif

	if emitterRef == None
		return false
	endif

	return mgr.IsEmitterReachableFromPlayer(emitterRef)
EndFunction

Bool Function PlayFxIfReachable(ObjectReference emitterRef, String fxBasename)
	if fxBasename == ""
		return false
	endif
	if !IsRadioReachableForFx(emitterRef)
		return false
	endif
	return RadioSFSENative.playFx(emitterRef, fxBasename)
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

Event OnInit()
	mgr = myQuest as STAR_Start_Quest_Script
	BlockActivation(true)
EndEvent

Event OnLoad()
	; Keep default pickup blocked; explicit option 6 performs default processing.
	BlockActivation(true)
EndEvent

Bool Function ShowMenuAndExecute(ObjectReference akActionRef = None, Bool fromWorldActivate = False)
    if akActionRef == None
        akActionRef = Game.GetPlayer()
    endif

    if akActionRef != Game.GetPlayer()
        return false
    endif

    if !EnsureManager()
        Notify("Quest script missing.", true)
        return false
    endif

    if !mgr.canUseRadioControls()
        Notify("Build or buy a radio first.", true)
        return false
    endif

    if MyMenuMessage == None
        Notify("Radio menu not configured.", true)
        return false
    endif

    ObjectReference emitterRef = None
    if fromWorldActivate
        ObjectReference previousEmitter = mgr.getEmitter()
        if previousEmitter != self
            if previousEmitter != None
                mgr.CapturePersistentState(previousEmitter)
            endif
            mgr.setEmitter(self)
            mgr.UpdateLastRadioLocation(self)
            mgr.ApplyPersistentStateToEmitter(self)
        endif
        emitterRef = self
    else
        emitterRef = mgr.ResolveEmitterForControls()
    endif

    if emitterRef == None
        Notify("No radio device.", true)
        return false
    endif

    ; 0-type, 1-playlist, 2-play/stop, 3-forward, 4-vol+, 5-vol-, 6-take
    int buttonID = MyMenuMessage.Show()
    if emitterRef != mgr.getEmitter() && buttonID != 6 && !RadioSFSENative.isPlaying(emitterRef)
        mgr.ApplyPersistentStateToEmitter(emitterRef)
    endif

    if buttonID == 0
        int mediaType = mgr.getMediaType()
        if mediaType >= 3
            mediaType = 1
        else
            mediaType = mediaType + 1
        endif

        mgr.setMediaType(mediaType, emitterRef)
        String sourceName = RadioSFSENative.currentSourceName(emitterRef)
        PlayFxIfReachable(emitterRef, "tuning_short.mp3")

        if sourceName == ""
            String err0 = RadioSFSENative.lastError(emitterRef)
            if err0 == ""
                err0 = "Could not change media type."
            endif
            Notify(err0, true)
        else
            Notify("Media type "+mediaType+": "+sourceName, True)
        endif
        mgr.CapturePersistentState(emitterRef)

    elseif buttonID == 1
        PlayFxIfReachable(emitterRef, "tuning_short.mp3")
        int mediaType2 = mgr.getMediaType()
        Bool selected = RadioSFSENative.selectNextSource(emitterRef, mediaType2)
        if !selected
            String err1 = RadioSFSENative.lastError(emitterRef)
            if err1 == ""
                err1 = "Could not change playlist/source."
            endif
            Notify(err1, true)
        else
            String sourceName2 = RadioSFSENative.currentSourceName(emitterRef)
            Notify("Selected: "+sourceName2, True)
        endif
        mgr.CapturePersistentState(emitterRef)

    elseif buttonID == 2
        if RadioSFSENative.isPlaying(emitterRef)
            RadioSFSENative.pause(emitterRef)
            String err2p = RadioSFSENative.lastError(emitterRef)
            if err2p != ""
                Notify(err2p, true)
            else
                Notify("Playback paused.")
            endif
        else
            if mgr.getMediaType() == 3
                PlayFxIfReachable(emitterRef, "tuning_long.mp3")
            endif
            RadioSFSENative.play(emitterRef)
            if WaitForPlaybackResult(emitterRef)
                String nowName = RadioSFSENative.currentTrackBasename(emitterRef)
                if nowName == ""
                    nowName = RadioSFSENative.currentSourceName(emitterRef)
                endif
                Notify("Now playing: "+nowName, True)
            else
                String err2 = RadioSFSENative.lastError(emitterRef)
                if err2 == ""
                    err2 = "Could not start playback."
                endif
                PlayFxIfReachable(emitterRef, "no_station.mp3")
                Notify(err2, true)
            endif
        endif
        mgr.CapturePersistentState(emitterRef)

    elseif buttonID == 3
        RadioSFSENative.forward(emitterRef)
        String err3 = RadioSFSENative.lastError(emitterRef)
        if err3 != "" && !RadioSFSENative.isPlaying(emitterRef)
            Utility.Wait(0.3)
            RadioSFSENative.forward(emitterRef)
            err3 = RadioSFSENative.lastError(emitterRef)
        endif
        if err3 != ""
            Notify(err3, true)
        else
            String nextName = RadioSFSENative.currentTrackBasename(emitterRef)
            if nextName == ""
                nextName = RadioSFSENative.currentSourceName(emitterRef)
            endif
            Notify("Now playing: "+nextName, True)
        endif
        mgr.CapturePersistentState(emitterRef)

    elseif buttonID == 4
        Bool volUpOk = RadioSFSENative.volumeUp(emitterRef, VolumeStep)
        if !volUpOk
            String err4 = RadioSFSENative.lastError(emitterRef)
            if err4 == ""
                err4 = "Could not increase volume."
            endif
            Notify(err4, true)
        else
            Float volUp = RadioSFSENative.getVolume(emitterRef)
            Notify("Volume: "+volUp, True)
        endif
        mgr.CapturePersistentState(emitterRef)

    elseif buttonID == 5
        Bool volDownOk = RadioSFSENative.volumeDown(emitterRef, VolumeStep)
        if !volDownOk
            String err5 = RadioSFSENative.lastError(emitterRef)
            if err5 == ""
                err5 = "Could not decrease volume."
            endif
            Notify(err5, true)
        else
            Float volDown = RadioSFSENative.getVolume(emitterRef)
            Notify("Volume: "+volDown, True)
        endif
        mgr.CapturePersistentState(emitterRef)

    elseif buttonID == 6
        ; Explicit pickup path only.
        Notify("Taking radio into inventory.")
        self.Activate(Game.GetPlayer(), true)
    endif

    return true
EndFunction

Event OnActivate(ObjectReference akActionRef)
    ShowMenuAndExecute(akActionRef, true)
EndEvent


Event OnContainerChanged(ObjectReference akNewContainer, ObjectReference akOldContainer)
	if !EnsureManager()
		return
	endif

	Actor player = Game.getPlayer()
	if player == None
		return
	endif

	ObjectReference currentEmitter = mgr.getEmitter()
	int carriedRadiosNow = mgr.GetCarriedRadioCount()
	Trace("OnContainerChanged: self=" + self + " new=" + akNewContainer + " old=" + akOldContainer + " currentEmitter=" + currentEmitter + " carried=" + carriedRadiosNow)

	ObjectReference targetEmitter = currentEmitter
	if akNewContainer == None
		; Last dropped radio always becomes the main world radio.
		targetEmitter = self
		Trace("OnContainerChanged branch: dropped to world -> target=self")
	elseif akNewContainer == player
		; Picking up the active radio (or with no active radio) shifts control to inventory radio.
		if currentEmitter == self || currentEmitter == None || currentEmitter == player
			targetEmitter = player
			Trace("OnContainerChanged branch: moved to player -> target=player")
		else
			Trace("OnContainerChanged: ignored pickup because current emitter is another world radio.")
			return
		endif
	else
		; Moving radios to other containers must not hijack an unrelated in-world main radio.
		if currentEmitter != self
			Trace("OnContainerChanged: ignored transfer to non-player container (active emitter differs).")
			return
		endif

		if carriedRadiosNow > 0
			targetEmitter = player
			Trace("OnContainerChanged branch: transferred to container, fallback to player emitter.")
		else
			targetEmitter = None
			Trace("OnContainerChanged branch: transferred to container, no radios carried -> clear emitter.")
		endif
	endif
	Trace("OnContainerChanged targetEmitter=" + targetEmitter)

	if currentEmitter != None && currentEmitter != targetEmitter
		Trace("OnContainerChanged: stopping previous emitter " + currentEmitter)
		mgr.CapturePersistentState(currentEmitter)
		RadioSFSENative.stop(currentEmitter)
	endif

	if targetEmitter == None
		Trace("OnContainerChanged: clearing manager emitter and last location.")
		mgr.RadioEmitter = None
		mgr.lastRadioWorldspace = None
		mgr.lastRadioCell = None
		return
	endif

	BlockActivation(true)
	mgr.setEmitter(targetEmitter)
	if targetEmitter == player
		Trace("OnContainerChanged: emitter switched to player.")
		mgr.lastRadioWorldspace = None
		mgr.lastRadioCell = None
	else
		Trace("OnContainerChanged: emitter switched to world ref " + targetEmitter)
		mgr.UpdateLastRadioLocation(targetEmitter)
	endif
	mgr.ApplyPersistentStateToEmitter(targetEmitter)
	mgr.CapturePersistentState(targetEmitter)
	Trace("OnContainerChanged: persistent state applied to target emitter.")
EndEvent
