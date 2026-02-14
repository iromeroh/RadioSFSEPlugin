Scriptname RadioSFSE_StopSlateScript extends ObjectReference

Event OnActivate(ObjectReference akActionRef)
	if akActionRef != Game.GetPlayer()
		return
	endif

	RadioSFSENative.stop(Self)
EndEvent
