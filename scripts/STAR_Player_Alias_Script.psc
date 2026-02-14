Scriptname STAR_Player_Alias_Script extends ReferenceAlias

STAR_Start_Quest_Script Function GetMgr()
        return GetOwningQuest() as STAR_Start_Quest_Script
EndFunction

Event OnPlayerLoadGame()
        STAR_Start_Quest_Script mgr = GetMgr()
        mgr.Notify("Game loaded", True)
        if mgr
                mgr.RefreshControlSlateAccess()
                mgr.RestorePersistentState()
        endif
EndEvent