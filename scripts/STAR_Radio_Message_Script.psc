Scriptname STAR_Radio_Message_Script extends ObjectReference


; Define the message box/menu items
Message Property MyMenuMessage Auto
Bool Property VerboseNotifications = False Auto

Event OnActivate(ObjectReference akActionRef)
    if (akActionRef == Game.GetPlayer())
        ; Show the message box, 0 is default button
        int buttonID = MyMenuMessage.Show()
        
        if (buttonID == 0)
            if VerboseNotifications
                Debug.Notification("Option 1 Selected")
            endif
            ; Add functionality for option 1
        elseif (buttonID == 1)
            if VerboseNotifications
                Debug.Notification("Option 2 Selected")
            endif
            ; Add functionality for option 2
        endif
    endif
EndEvent
