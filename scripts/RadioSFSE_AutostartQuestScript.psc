Scriptname STAR_Start_Quest_Script extends Quest

ObjectReference Property RadioEmitter Auto
String Property StartupPlaylist = "Playlist_Default" Auto
Bool Property AutoStartPlayback = True Auto
Bool Property UseStationStart = False Auto
Bool Property UpdateFade = True Auto
Float Property FadeUpdateSeconds = 1.0 Auto
int MyTimer = 528

Event OnInit()
	InitializeRadio()
EndEvent

Function InitializeRadio()
	ObjectReference emitterRef = RadioEmitter
	if emitterRef == None
		emitterRef = Game.GetPlayer()
	endif

	if StartupPlaylist != ""
		RadioSFSENative.change_playlist(emitterRef, StartupPlaylist)
	endif

	if AutoStartPlayback
		if UseStationStart
			RadioSFSENative.start(emitterRef)
		else
			RadioSFSENative.play(emitterRef)
		endif
	endif

	PushFadeSample()

	if UpdateFade
		; RegisterForSingleUpdate(FadeUpdateSeconds)
                StartTimer(FadeUpdateSeconds, MyTimer)

	endif
EndFunction

Event OnTimer(int aiTimerID)
        ; Debug.Notification("OnTimer().")
        Debug.Trace("FYM_MainQuestScrip: OnTimer().")

        if aiTimerID != MyTimer
            return  ; Ignore if wrong ID or stopped
        endif

	PushFadeSample()
	if UpdateFade
		; RegisterForSingleUpdate(FadeUpdateSeconds)
                StartTimer(FadeUpdateSeconds, MyTimer)
	endif
EndEvent

Function PushFadeSample()
	ObjectReference playerRef = Game.GetPlayer()
	if playerRef == None
		return
	endif

	ObjectReference emitterRef = RadioEmitter
	if emitterRef == None
		emitterRef = playerRef
	endif

	RadioSFSENative.set_positions( emitterRef,emitterRef.GetPositionX(),emitterRef.GetPositionY(),emitterRef.GetPositionZ(),playerRef.GetPositionX(),playerRef.GetPositionY(),	playerRef.GetPositionZ(), playerRef.GetAngleZ())
EndFunction
