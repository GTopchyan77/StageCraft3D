# MIDI and OSC: running the desk from a controller or a tablet
keywords: midi, osc, remote, controller, fader box, touchosc, open stage control, tablet, nanokontrol, learn, feedback, external control
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

The status line under the choices shows the last message received, so you can check which control sends what.
