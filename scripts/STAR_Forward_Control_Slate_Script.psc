Scriptname STAR_Forward_Control_Slate_Script extends ObjectReference

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
        mgr.Trace("STAR_Forward_Control_Slate: " + text)
    endif
EndFunction

Event OnRead()
    Notify("Play Slate OnRead()")
    mgr = myQuest as STAR_Start_Quest_Script
    if !mgr
        Notify("Quest script missing.", true)
        Debug.Trace("STAR_Forward_Control_Slate: STAR_Start_Quest_Script missing on quest" + myQuest)
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

    if mgr.RadioIsPlaying(emitterRef)

        ; Debug.Notification("Forward.")

        Trace("Forward playback.")
        mgr.RadioForward(emitterRef)
        String err = mgr.RadioLastError(emitterRef)
        if err != "" && !mgr.RadioIsPlaying(emitterRef)
            Utility.Wait(0.3)
            mgr.RadioForward(emitterRef)
            err = mgr.RadioLastError(emitterRef)
        endif
        if mgr.RadioIsPlaying(emitterRef)
            String mediaName = mgr.RadioCurrentTrackBasename(emitterRef)
            if mediaName == ""
                mediaName = mgr.RadioCurrentSourceName(emitterRef)
            endif
            Notify("Now playing: "+mediaName)
        else
            if err == ""
                err = "Could not skip/advance playback."
            endif
            Notify(err, true)
            Trace("forward failed: " + err)
        endif
        mgr.CapturePersistentState(emitterRef)


    endif
EndEvent


