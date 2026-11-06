# Cues, tracking and store modes
keywords: cue, cues, store, record, track, tracking, cue only, look, block, blocked, inherit, cue list
panel: StageCraft.Panel.Cues
control: ModeTrack

A cue list tracks: each cue holds only what it changes, and the cues before it give the rest. GO plays the next cue with its timing.

To record a cue, set up the look in the programmer (select fixtures and change them), type the cue number in the Cues panel's CUE field, pick a store mode and press STORE.
- TRACK stores what the programmer holds; later cues keep it until they change it.
- CUE ONLY stores it in this cue only; the next cue puts the old values back.
- LOOK stores every fixture's whole look as a blocked cue that inherits nothing.

BLK on a cue's row blocks it: it keeps its look but stops inheriting from the cues before it. A decimal cue number (2.5) inserts a cue between two others. Every store is one undo step (Ctrl+Z).
