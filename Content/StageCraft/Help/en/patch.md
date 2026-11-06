# Patching fixtures: IDs, universes and addresses
keywords: patch, patching, address, dmx address, universe, fixture id, fid, unpatched, conflict, overlap, auto patch, fix conflicts, patch selection, csv, schedule
panel: StageCraft.Panel.Patch
control: AutoPatch

The Patch panel lists every fixture with its fixture ID, universe and DMX start address. A fixture occupies as many channels as its DMX mode has; universe 0 means unpatched.

- AUTO PATCH patches every unpatched fixture into the first free addresses that fit, in fixture ID order.
- Overlapping addresses are marked in red. FIX CONFLICTS moves every fixture that overlaps another (or runs past channel 512) to free addresses; in each overlap the fixture that starts first stays.
- PATCH SELECTION AT patches the selected fixtures one after another from the universe and address you type.
- IMPORT CSV patches from a fixture schedule; you see the changes before APPLY. EXPORT PDF writes the patch sheet.

Every patch change is one undo step.
