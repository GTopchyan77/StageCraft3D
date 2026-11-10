# Importing an MVR scene
keywords: mvr, my virtual rig, import mvr, lighting plot, plot, vectorworks, grandma3, capture, wysiwyg, depence, rig, focus point, layer, scene exchange
panel: StageCraft.Panel.Library

An MVR file (.mvr) is a whole lighting rig from a design or console application such as Vectorworks Spotlight, grandMA3, Capture or Depence: the fixtures with their GDTF types, DMX patch, IDs and positions, the trusses and the focus points. StageCraft reads it and builds the rig on your stage.

- Import: File > Import MVR Scene... and pick the .mvr file, or drag it from Explorer onto the viewport.
- Before anything changes you see what the file holds: its layers, fixtures, trusses and focus points, and each fixture type. A type is "new" (added to the Library), "in the Library" (your copy is used) or "cannot be used". Fixtures whose type or DMX mode cannot be used are placed as the Sketch window's generic fixture and named in the notes. IMPORT, or CANCEL.
- Fixtures keep their position, their DMX address (universe and address) and their fixture ID when it is free; a taken ID gets the next free one. A fixture with a focus point aims at it (pan and tilt, within what it can reach).
- Trusses become truss runs of whole sections along the length of the file's truss, built with the Sketch window's truss. Upright towers, set pieces, supports and video screens are listed but not placed in this version.
- Each layer becomes a fixture group with the layer's name.
- The whole import is one undo step (Ctrl+Z). The fixture types it added stay in the Library.
- Importing the same file again does not place its fixtures twice: items already on the stage are skipped.
- Big rigs are placed a little each frame, so StageCraft keeps responding; the review shows the progress.
