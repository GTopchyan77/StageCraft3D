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
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Panel_Preferences,	"StageCraft.Panel.Preferences",	"Preferences: keyboard shortcuts, camera look, viewport and audio.");
}
