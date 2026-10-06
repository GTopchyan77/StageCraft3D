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
}
