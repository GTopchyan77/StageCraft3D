# Exporting an MVR scene
keywords: mvr, my virtual rig, export mvr, save mvr, lighting plot, plot, vectorworks, grandma3, capture, wysiwyg, depence, rig, layer, scene exchange
panel: StageCraft.Panel.Library

Export writes your stage as an MVR file (.mvr), which Vectorworks Spotlight, grandMA3, Capture, Depence and other MVR applications open. Use it to hand your rig to the lighting designer's plot or to the console.

- Export: File > Export MVR Scene..., then choose where to save the file. The status bar says what was written.
- Fixtures keep their position, DMX address, fixture ID and name. A fixture from your Library's GDTF types is written with its GDTF file and DMX mode, so the other application knows exactly which fixture it is. Built-in StageCraft fixtures have no GDTF file and are written by name and patch only.
- Each fixture group with fixtures on the stage becomes a layer with the group's name; a fixture in several groups goes to the first. Everything else is on a layer named "StageCraft".
- Trusses are written as trusses, one per straight run, with a simple box for their shape. Other items (decks, set pieces, speakers) are written as boxes of their size, so the plot shows where they stand.
- Fixtures and trusses that came from an MVR import keep their MVR ids, and everything else keeps the same id each time you export, so the other application can recognise items it has seen before.
- Not written in this version: focus points, video screens, the fixtures' pan and tilt, and the real shapes of trusses and set pieces.
- Your stage does not change, and an export cannot be undone because nothing on the stage moved.
