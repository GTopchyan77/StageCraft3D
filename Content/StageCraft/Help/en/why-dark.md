# Why is a fixture dark?
keywords: dark, no light, not lighting, off, black, nothing happens, fixture dark, light not working, troubleshoot, problem
panel: StageCraft.Panel.Playback
control: GrandMaster

When a fixture stays dark, check in this order:
1. BLACKOUT is off and the Grand Master is up (Playback panel).
2. Its dimmer is above 0 in the programmer, the playing cue or a playback; a tracking cue may still hold 0 from an earlier cue.
3. The show is INTERNAL (Cues panel). Under EXTERNAL DMX the console on the network drives it.
4. For a real fixture on DMX OUT: it is patched (universe above 0, no red conflict in the Patch panel), DMX OUT is on and its universe is not muted.
5. An effect may be dipping it, or a strobe may hold its shutter.

HIGHLIGHT (Groups panel) shows the selected fixtures at full to find them.
