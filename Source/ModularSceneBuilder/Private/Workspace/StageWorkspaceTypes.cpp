// Copyright Epic Games, Inc. All Rights Reserved.

#include "Workspace/StageWorkspaceTypes.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageWorkspaceTypes)

DEFINE_LOG_CATEGORY(LogStageWorkspace);

namespace StageCraftTags
{
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Panel,			"StageCraft.Panel",				"Root of all workspace panels. A panel's tag name is its dock tab ID.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Panel_Viewport,	"StageCraft.Panel.Viewport",	"The 3D game viewport. Cannot be closed; can be floated to another monitor.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Panel_Inspector,	"StageCraft.Panel.Inspector",	"Parameter inspector for the selected item.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Panel_FaderBank,	"StageCraft.Panel.FaderBank",	"Fader and encoder bank for the selected item.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Panel_Library,		"StageCraft.Panel.Library",		"Item library: pick a catalog item to place.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Panel_Render,		"StageCraft.Panel.Render",		"Render panel: high-resolution stills of the current view.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Panel_Outliner,		"StageCraft.Panel.Outliner",	"Outliner: placed items by discipline, with visibility, lock and group selection.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Panel_BillOfMaterials, "StageCraft.Panel.BillOfMaterials", "Bill of materials: quantities, weight and power of everything on stage, with CSV export.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Panel_Patch,		"StageCraft.Panel.Patch",		"DMX patch sheet: fixture IDs, universes and addresses, conflicts, and the selected fixture's channels.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Panel_Environment, "StageCraft.Panel.Environment", "Environment panel: outdoor sky, time of day, sun path and weather.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Panel_Cues,		"StageCraft.Panel.Cues",		"Cue list: store, play and edit the show's lighting cues; internal or external DMX control.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Panel_Groups,		"StageCraft.Panel.Groups",		"Groups and programmer: fixture groups, Clear, Highlight and the fan mode (ADR 0025).");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Panel_Palettes,	"StageCraft.Panel.Palettes",	"Palette pools: colour, position, beam, focus and intensity looks that cues refer to (ADR 0028).");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Panel_Playback,	"StageCraft.Panel.Playback",	"Playback desk: ten playback faders with Go, Flash, Record and Release, the Grand Master and Blackout (ADR 0026).");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Panel_Effects,		"StageCraft.Panel.Effects",		"Effects: quick effects on the selection, the running effects and their rate, size, phase, spread and shape (ADR 0027).");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Panel_Audio,		"StageCraft.Panel.Audio",		"Audio: the music audio effects follow (file, input, system audio, test beat), its transport, band meters, tempo and beat (ADR 0029).");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Panel_Preferences,	"StageCraft.Panel.Preferences",	"Preferences: keyboard shortcuts, camera look, viewport and audio.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Panel_Sketch,		"StageCraft.Panel.Sketch",		"Sketch to Stage: draw decks, truss lines and fixtures in 2D and generate them in 3D.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Panel_Assistant,	"StageCraft.Panel.Assistant",	"AI Assistant: ask about the stage and request safe, undoable changes in plain language.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Panel_Marketplace,	"StageCraft.Panel.Marketplace",	"Community Marketplace: browse, preview in 3D, get and publish stages (ADR 0021).");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Panel_SendTo,		"StageCraft.Panel.SendTo",		"Send to 3D App: send saved stages to Blender, 3ds Max or Cinema 4D, or save them as .glb (ADR 0022).");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Panel_Services,	"StageCraft.Panel.Services",	"Services & Keys: API keys of AI tools (encrypted per user) and the 3D apps connected over the bridge (ADR 0023).");
}
