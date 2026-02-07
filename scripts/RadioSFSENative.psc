Scriptname RadioSFSENative Hidden

; Intended native API surface for SFSE binding.
; Current milestone captures VM registration context via direct hook but
; these natives are not bound until registration ABI is finalized.

Function change_playlist(ObjectReference activatorRef, String channelName) Global Native
Function play(ObjectReference activatorRef) Global Native
Function start(ObjectReference activatorRef) Global Native
Function pause(ObjectReference activatorRef) Global Native
Function stop(ObjectReference activatorRef) Global Native
Function forward(ObjectReference activatorRef) Global Native
Function rewind(ObjectReference activatorRef) Global Native

; Activator/player positional data feed for fade calculations.
Function set_positions(ObjectReference activatorRef, Float activatorX, Float activatorY, Float activatorZ, Float playerX, Float playerY, Float playerZ) Global Native
