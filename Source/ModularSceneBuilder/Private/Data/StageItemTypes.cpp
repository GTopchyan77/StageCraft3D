// Copyright Epic Games, Inc. All Rights Reserved.

#include "Data/StageItemTypes.h"

namespace StageCraftTags
{
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Category,			"StageCraft.Category",			"Root of all stage item catalog categories.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Category_Stage,		"StageCraft.Category.Stage",	"Decks, risers, ramps and other stage structure.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Category_Truss,		"StageCraft.Category.Truss",	"Truss segments, towers and rigging.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Category_Audio,		"StageCraft.Category.Audio",	"Speakers, line arrays and monitors.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Category_Screen,		"StageCraft.Category.Screen",	"LED walls and projection surfaces.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Category_Lighting,	"StageCraft.Category.Lighting",	"Moving heads, pars, washes and other fixtures.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Category_Prop,		"StageCraft.Category.Prop",		"Instruments, furniture and set dressing.");

	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Category_Stage_Deck,				"StageCraft.Category.Stage.Deck",			"Modular stage decks (e.g. 2x1 m platforms).");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Category_Stage_Riser,			"StageCraft.Category.Stage.Riser",			"Drum and band risers, steps and ramps.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Category_Truss_Straight,			"StageCraft.Category.Truss.Straight",		"Straight truss segments.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Category_Truss_Corner,			"StageCraft.Category.Truss.Corner",			"Corners, T-pieces and boxes.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Category_Truss_Tower,			"StageCraft.Category.Truss.Tower",			"Ground-support towers and sleeve blocks.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Category_Rigging,				"StageCraft.Category.Rigging",				"Hoists, bridles, spansets and hanging hardware.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Category_Rigging_Motor,			"StageCraft.Category.Rigging.Motor",		"Chain hoists.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Category_Lighting_MovingHead,	"StageCraft.Category.Lighting.MovingHead",	"Spot, beam, wash and hybrid moving heads.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Category_Lighting_Par,			"StageCraft.Category.Lighting.Par",			"LED pars and static washes.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Category_Lighting_Strobe,		"StageCraft.Category.Lighting.Strobe",		"Strobes and blinders.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Category_Lighting_Laser,			"StageCraft.Category.Lighting.Laser",		"Show lasers.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Category_Audio_LineArray,		"StageCraft.Category.Audio.LineArray",		"Line array elements and hang frames.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Category_Audio_Subwoofer,		"StageCraft.Category.Audio.Subwoofer",		"Subwoofers.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Category_Audio_Monitor,			"StageCraft.Category.Audio.Monitor",		"Wedges, side fills and front fills.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Category_Audio_Console,			"StageCraft.Category.Audio.Console",		"FOH and monitor mixing consoles.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Category_Audio_Amplifier,		"StageCraft.Category.Audio.Amplifier",		"Amplifier racks and system processors.");
}
