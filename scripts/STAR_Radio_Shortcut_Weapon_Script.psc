Scriptname STAR_Radio_Shortcut_Weapon_Script extends ObjectReference

Quest Property myQuest Auto Mandatory ; -> STAR_Start_Quest
STAR_Start_Quest_Script Property mgr Auto
Bool Property UnequipAfterUse = True Auto
Float Property UnequipDelaySeconds = 0.1 Auto

Bool Function EnsureManager()
	if mgr == None
		mgr = myQuest as STAR_Start_Quest_Script
	endif
	return mgr != None
EndFunction

Function OpenRadioMenu()
	if !EnsureManager()
		Debug.Notification("Radio quest script missing.")
		return
	endif

	mgr.RequestOpenControlTerminal()
EndFunction

Event OnEquipped(Actor akActor)
	if akActor != Game.GetPlayer()
		return
	endif

	OpenRadioMenu()

	if UnequipAfterUse
		if UnequipDelaySeconds > 0.0
			Utility.Wait(UnequipDelaySeconds)
		endif
		akActor.UnequipItem(self.GetBaseObject(), false, true)
	endif
EndEvent

Event OnActivate(ObjectReference akActionRef)
	if akActionRef != Game.GetPlayer()
		return
	endif

	OpenRadioMenu()
EndEvent
