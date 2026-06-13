Scriptname STAR_Play_Stop_Control_Slate_Script extends ObjectReference

Quest Property myQuest Auto Mandatory ; -> STAR_Start_Quest
STAR_Start_Quest_Script Property mgr Auto

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

;Event OnLoad()
;    mgr = myQuest as STAR_Start_Quest_Script
;    mgr.setMediaType(mgr.getMediaType())
;    Debug.Notification("Setting startup playlist")
;EndEvent


Event OnRead()
    mgr = myQuest as STAR_Start_Quest_Script
    if !mgr
        Notify("Quest script missing.", true)
        return
    endif

    if !mgr.canUseRadioControls()
        Notify("Build or buy a radio first.", true)
        return
    endif

    mgr.RequestOpenControlTerminal()
EndEvent

