Scriptname STAR_Player_Alias_Script extends ReferenceAlias

STAR_Start_Quest_Script Function GetMgr()
        return GetOwningQuest() as STAR_Start_Quest_Script
EndFunction

Event OnPlayerLoadGame()
        STAR_Start_Quest_Script mgr = GetMgr()
        if mgr
                mgr.ResetSFSEProbeState()
                mgr.Notify("Game loaded", True)
                mgr.RefreshControlSlateAccess()
                mgr.RestorePersistentState()
                mgr.SyncCellMusicMute()
        endif
EndEvent

Event OnItemEquipped(Form akBaseObject, ObjectReference akReference)
        STAR_Start_Quest_Script mgr = GetMgr()
        if mgr == None
                return
        endif

        if mgr.RadioShortcutWeapon != None && akBaseObject == mgr.RadioShortcutWeapon
                mgr.RequestOpenControlTerminal()
        endif
EndEvent
