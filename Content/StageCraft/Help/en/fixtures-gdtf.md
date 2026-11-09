# Importing fixtures (GDTF)
keywords: gdtf, import fixture, fixture file, fixture library, device library, manufacturer, personality, dmx mode, mode, footprint, gdtf-share, replace fixture, remove fixture, kind
panel: StageCraft.Panel.Library

A GDTF file (.gdtf) describes a real fixture: its DMX modes, channels, wheels and colours. Manufacturers publish them, for example on gdtf-share.com. StageCraft turns each file into a Library fixture you can place, patch, program and send to the real rig over DMX OUT.

- Import: IMPORT in the Library (or File > Import Fixtures (GDTF)...) and pick the .gdtf file, or drag it from Explorer onto the Library or the viewport.
- Before anything is added you see the import report: the manufacturer and name, each DMX mode with its channel count, and how many channels StageCraft drives, drives approximately or only sends at their default value (Focus, Control and similar). Modes StageCraft cannot use are listed with the reason. Correct KIND if the guess is wrong, then ADD, or CANCEL.
- The fixture appears as one card in the Library with the number of modes. Click it to place it in its first mode. Switch the mode under Type in the inspector; every mode keeps the real footprint, and the change is one undo step.
- Importing a file of a fixture you already have shows REPLACE: placed fixtures move to the mode with the same name and keep their patch and programming.
- REMOVE on the card (click twice) deletes the fixture type. It is refused while fixtures of that type are on the stage.
- The file is copied into the Library, so the fixture is there at the next start. A scene that uses a fixture this PC does not have says which file to import.
