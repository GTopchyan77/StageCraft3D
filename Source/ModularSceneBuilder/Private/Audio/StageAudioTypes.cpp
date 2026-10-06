// Copyright Epic Games, Inc. All Rights Reserved.

#include "Audio/StageAudioTypes.h"

namespace StageCraftTags
{
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Sound,			"StageCraft.Sound",			"Root of editor feedback sound cues.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Sound_Select,	"StageCraft.Sound.Select",	"A placed item was selected.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Sound_Place,		"StageCraft.Sound.Place",	"An item was placed.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Sound_Snap,		"StageCraft.Sound.Snap",	"The placement preview snapped to a new grid point.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Sound_Error,		"StageCraft.Sound.Error",	"An action was refused or could not be done.");
}
