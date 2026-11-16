# MIDI and OSC: running the desk from a controller or a tablet
keywords: midi, osc, remote, controller, fader box, touchosc, open stage control, tablet, nanokontrol, learn, feedback, external control, midi map, mapping, motor fader, led, midi output
panel: StageCraft.Panel.Preferences

A MIDI fader box or an OSC app on a tablet can run the desk: cue GO and BACK, the ten playbacks (level, flash, GO, release), the Grand Master, Blackout and the Audio master. Each move goes through the same rules as a click on the desk, so a session that does not allow it refuses it. Remote moves are not undoable, like the desk.

Turn them on in Preferences › REMOTE CONTROL. Both are off until you choose.

**OSC** listens on port 8000:
- This PC: only apps on this computer.
- Network: a tablet on your network too. OSC has no password, so anyone on the network who knows the port can run the show.

Send a value from 0 to 1 to faders and masters; buttons fire on 1 (press) and ignore 0 (release):
- /stagecraft/cue/go, /stagecraft/cue/back, /stagecraft/cue/goto <cue number>
- /stagecraft/playback/1/level … /stagecraft/playback/10/level
- /stagecraft/playback/N/flash (1 down, 0 up), /stagecraft/playback/N/go, /stagecraft/playback/N/release
- /stagecraft/master/grand, /stagecraft/master/blackout (1 on, 0 off), /stagecraft/master/audio

The desk answers on port 9000 of the device that last sent to it, so a tablet's faders follow the desk: /stagecraft/cue/active, the playback levels and the masters.

**MIDI**: pick your controller in MIDI input (plug it in before opening Preferences). The default map follows the Korg nanoKONTROL2: faders 1–8 are playbacks 1–8, S flashes, M is playback GO, R releases, knob 1 is the Grand Master, knob 2 the Audio master, ▶ is GO, ◀◀ is BACK and ● is Blackout.

**MIDI map**: change which control does what under MIDI map. Each row shows the controls that drive it (CC 41, Note 60 ch 10):
- LEARN, then move or press a control on your MIDI input: it now drives that row, and only that row. Click LEARN again to stop waiting. Closing Preferences stops it too.
- CLEAR: nothing drives the row any more.
- Playback 1–10 picks the playback the Fader, Flash, GO and Release rows edit.
- RESET TO DEFAULT brings back the nanoKONTROL2 map; CLEAR ALL starts from an empty map.
Editing the map keeps your controller connected. Go to cue is not in the list; RESET TO DEFAULT or CLEAR ALL removes any you had.

**MIDI output** sends the show's state back to the controller: motorised faders move with the desk, and button lights show Blackout, a held flash and a live playback (its GO button). Choose the same controller as the input (it is listed once as an input and once as an output). A fader you are moving is never pushed back against your hand; if the desk refuses the move, the fader returns to the desk's level half a second after you let go. Some controllers light their buttons only when set to external LED control in their own editor (on the nanoKONTROL2: LED Mode External in KORG KONTROL Editor).

The status line under the choices shows the last message received, so you can check which control sends what.
- MIDI and OSC control is part of StageCraft Pro. Without a licence StageCraft does not open the OSC port or the MIDI devices, and when the licence runs out it closes them. Your settings and your MIDI map are kept: when Pro is back, OSC and MIDI open again by themselves. The status says why (Help: StageCraft Pro).
