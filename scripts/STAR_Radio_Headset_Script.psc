Scriptname STAR_Radio_Headset_Script extends ObjectReference

Quest Property myQuest Auto Mandatory ; -> STAR_Start_Quest
STAR_Start_Quest_Script Property mgr Auto

Bool Function EnsureManager()
	if mgr == None
		mgr = myQuest as STAR_Start_Quest_Script
	endif
	return mgr != None
EndFunction

Event OnEquipped(Actor akActor)
	if akActor != Game.GetPlayer()
		return
	endif

	if EnsureManager()
		mgr.OnRadioHeadsetEquipped()
	endif
EndEvent

Event OnUnequipped(Actor akActor)
	if akActor != Game.GetPlayer()
		return
	endif

	if EnsureManager()
		mgr.OnRadioHeadsetUnequipped()
	endif
EndEvent
