Scriptname RadioSFSENative Hidden Native


; Native API surface bound by RadioSFSE via CommonLibSF VM registration.

Function change_playlist(ObjectReference activatorRef, String channelName) Global Native
Function play(ObjectReference activatorRef) Global Native
Function start(ObjectReference activatorRef) Global Native
Function pause(ObjectReference activatorRef) Global Native
Function stop(ObjectReference activatorRef) Global Native
Function forward(ObjectReference activatorRef) Global Native
Function rewind(ObjectReference activatorRef) Global Native
Bool Function isPlaying(ObjectReference activatorRef) Global Native
String Function currentSourceName(ObjectReference activatorRef) Global Native
String Function currentTrackBasename(ObjectReference activatorRef) Global Native
Bool Function changeToNextSource(ObjectReference activatorRef, Int category) Global Native
Bool Function selectNextSource(ObjectReference activatorRef, Int category) Global Native
Bool Function setFadeParams(ObjectReference activatorRef, Float minDistance, Float maxDistance, Float panDistance) Global Native
Bool Function volumeUp(ObjectReference activatorRef, Float step) Global Native
Bool Function volumeDown(ObjectReference activatorRef, Float step) Global Native
Float Function getVolume(ObjectReference activatorRef) Global Native
Bool Function setVolume(ObjectReference activatorRef, Float vol) Global Native
String Function getTrack(ObjectReference activatorRef) Global Native
Bool Function setTrack(ObjectReference activatorRef, String trackBasename) Global Native

; Activator/player positional data feed for fade calculations.
Function set_positions(ObjectReference activatorRef, Float activatorX, Float activatorY, Float activatorZ, Float playerX, Float playerY, Float playerZ) Global Native
