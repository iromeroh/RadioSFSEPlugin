Scriptname RadioSFSE_PlaySlateScript ObjectReference

Quest Property myQuest Auto Mandatory ; -> STAR_Start_Quest
STAR_Start_Quest_Script Property mgr Auto

Bool playing = False

Event OnRead()
    Debug.Notification("Play Slate OnRead()")
    mgr = myQuest as STAR_Start_Quest_Script
    if !mgr
        Debug.Notification("Quest script missing.")
        Debug.Trace("STAR_Play_Stop_Control_Slate: STAR_Start_Quest_Script missing on quest" + myQuest)
        return
    endif
    Debug.Trace("STAR_Play_Stop_Control_Slate: Quest script cast successful.")

    Actor player = Game.getPlayer()
    ;Cell playerCell = player.GetParentCell()
    ObjectReference emitterRef = mgr.getEmitter()
    WorldSpace playerWS = player.getWorldSpace()
    WorldSpace radioWS = emitterRef.getWorldSpace()
    ; Cell radioCell = emitterRef.GetParentCell()

    if playerWS != radioWS
        Debug.Notification("Radio is not here.")
        return
    else
        Debug.Notification("Radio is nearby.")
    endif

    if RadioSFSENative.isPlaying(emitterRef)
	playing = True
    else
	playing = False
    endif

    if playing
        Debug.Notification("Stopping playback.")
        Debug.Trace("STAR_Play_Stop_Control_Slate: Stopping playback.")
        RadioSFSENative.pause(emitterRef)
    else
        RadioSFSENative.play(emitterRef)
        
        String mediaName = RadioSFSENative.currentTrackBasename(emitterRef)
        Debug.Notification("Now playing: "+mediaName)

    endif
EndEvent


