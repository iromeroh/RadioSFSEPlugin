Scriptname STAR_Play_Stop_Control_Slate_Script extends ObjectReference

Quest Property myQuest Auto Mandatory ; -> STAR_Start_Quest
STAR_Start_Quest_Script Property mgr Auto

Bool playing = False

Function Notify(String text, Bool force = False)
    if text == ""
        return
    endif
    if mgr
        mgr.Notify(text, force)
    elseif force
        Debug.Notification(text)
    endif
EndFunction

Function Trace(String text)
    if mgr
        mgr.Trace("STAR_Play_Stop_Control_Slate: " + text)
    endif
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

;Event OnLoad()
;    mgr = myQuest as STAR_Start_Quest_Script
;    mgr.setMediaType(mgr.getMediaType())
;    Debug.Notification("Setting startup playlist")
;EndEvent


Event OnRead()
    Notify("Play Slate OnRead()")
    mgr = myQuest as STAR_Start_Quest_Script
    if !mgr
        Notify("Quest script missing.", true)
        Debug.Trace("STAR_Play_Stop_Control_Slate: STAR_Start_Quest_Script missing on quest" + myQuest)
        return
    endif

    if !mgr.canUseRadioControls()
        Notify("Build or buy a radio first.", true)
        return
    endif
    ; Debug.Trace("STAR_Play_Stop_Control_Slate: Quest script cast successful.")

	    Actor player = Game.getPlayer()
	    ;Cell playerCell = player.GetParentCell()
	    ObjectReference emitterRef = mgr.ResolveEmitterForControls()
	    if ! emitterRef
	            Notify("No radio device.", true)
	            return
	    endif
	    int carriedRadios = player.getItemCount(mgr.permanentRadioEmitter.getBaseObject())
	    if !mgr.IsEmitterReachableFromPlayer(emitterRef)
	        Notify("Radio is not here.", true)
	        return
    elseif carriedRadios > 0
        Notify("Radio in inventory.")
    else
        Notify("Radio is nearby.")
    endif

    STAR_radio_script radioMenuScript = None
    ObjectReference radioMenuRef = mgr.permanentRadioEmitter
    if radioMenuRef == None && emitterRef != player
        radioMenuRef = emitterRef
    endif
    if radioMenuRef != None
        radioMenuScript = radioMenuRef as STAR_radio_script
    endif

    if radioMenuScript != None
        Bool menuOk = radioMenuScript.ShowMenuAndExecute(player)
        if !menuOk
            Trace("menu call returned false.")
        endif
        return
    endif

    if RadioSFSENative.isPlaying(emitterRef)
        Notify("Stopping playback.")
        Trace("Stopping playback.")
        RadioSFSENative.pause(emitterRef)
        mgr.CapturePersistentState(emitterRef)
    else
        if mgr.getMediaType() == 3
            RadioSFSENative.playFx(emitterRef, "tuning_long")
        endif
        RadioSFSENative.play(emitterRef)
        if WaitForPlaybackResult(emitterRef)
            String mediaName = RadioSFSENative.currentTrackBasename(emitterRef)
            if mediaName == ""
                mediaName = RadioSFSENative.currentSourceName(emitterRef)
            endif
            Notify("Now playing: "+mediaName)
            Trace("playing " + mediaName)
        else
            String err = RadioSFSENative.lastError(emitterRef)
            if err == ""
                err = "Could not start playback."
            endif
            RadioSFSENative.playFx(emitterRef, "no_station")
            Notify(err, true)
            Trace("play failed: " + err)
        endif
        mgr.CapturePersistentState(emitterRef)

    endif
EndEvent


