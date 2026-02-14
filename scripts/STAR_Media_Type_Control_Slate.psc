Scriptname STAR_Media_Type_Control_Slate extends ObjectReference

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
        mgr.Trace("STAR_Media_Type_Control_Slate: " + text)
    endif
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
    if mediaType == 3
         mediaType = 1
    else
         mediaType = mediaType + 1
    endif

	    mgr.setMediaType(mediaType, emitterRef)

    Notify("Setting media type to: "+mediaType)

    String mediaName = RadioSFSENative.currentSourceName(emitterRef)
    Notify("Selected media: "+mediaName)
    Notify("Press Play to start.")



EndEvent


