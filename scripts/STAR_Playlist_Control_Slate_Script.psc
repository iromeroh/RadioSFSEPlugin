Scriptname STAR_Playlist_Control_Slate_Script extends ObjectReference

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
        mgr.Trace("STAR_Playlist_Control_Slate: " + text)
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

Event OnRead()
    Notify("Media Type Slate OnRead()")
    mgr = myQuest as STAR_Start_Quest_Script
    if !mgr
        Notify("Quest script missing.", true)
        Debug.Trace("STAR_Media_Type_Control_Slate: STAR_Start_Quest_Script missing on quest" + myQuest)
        return
    endif

    if !mgr.canUseRadioControls()
        Notify("Build or buy a radio first.", true)
        return
    endif
    Trace("Quest script cast successful.")

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

    
   int mediaType = mgr.getMediaType()
   RadioSFSENative.playFx(emitterRef, "tuning_short")
   Bool selected = RadioSFSENative.selectNextSource(emitterRef, mediaType)
   if !selected
        String selErr = RadioSFSENative.lastError(emitterRef)
        if selErr == ""
            selErr = "Could not select source for this media type."
        endif
        Notify(selErr, true)
        Trace("selectNextSource failed: " + selErr)
        return
   endif

   String mediaName = RadioSFSENative.currentSourceName(emitterRef)
   if mediaType == 3
        RadioSFSENative.playFx(emitterRef, "tuning_long")
   endif
   RadioSFSENative.play(emitterRef)
   if WaitForPlaybackResult(emitterRef)
        Notify("Now playing: "+mediaName)
   else
        String err = RadioSFSENative.lastError(emitterRef)
        if err == ""
            err = "Could not start selected source."
        endif
        RadioSFSENative.playFx(emitterRef, "no_station")
        Notify(err, true)
        Trace("play failed: " + err)
   endif
   mgr.CapturePersistentState(emitterRef)


EndEvent

