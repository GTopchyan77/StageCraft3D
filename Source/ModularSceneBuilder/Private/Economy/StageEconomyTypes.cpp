// Copyright Epic Games, Inc. All Rights Reserved.

#include "Economy/StageEconomyTypes.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageEconomyTypes)

namespace StageCraftTags
{
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Currency,			"StageCraft.Currency",				"Root of all currencies.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Currency_Credits,	"StageCraft.Currency.Credits",		"Default soft currency.");

	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Entitlement,			"StageCraft.Entitlement",			"Root of everything a player can own.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Entitlement_Item,	"StageCraft.Entitlement.Item",		"Ownership of a catalog item (placing it, switching to it).");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Entitlement_Feature,	"StageCraft.Entitlement.Feature",	"Ownership of a feature or parameter (editing it).");

	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Entitlement_Pro,					"StageCraft.Entitlement.Pro",				"StageCraft Pro capabilities, granted only by a verified licence token (Docs/ADR/0034 §17).");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Entitlement_Pro_DmxUniverses,	"StageCraft.Entitlement.Pro.DmxUniverses",	"DMX OUT on more than one universe.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Entitlement_Pro_DmxIn,			"StageCraft.Entitlement.Pro.DmxIn",			"Art-Net / sACN input (DMX IN).");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Entitlement_Pro_MidiOsc,			"StageCraft.Entitlement.Pro.MidiOsc",		"MIDI and OSC remote control.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Entitlement_Pro_WebRemote,		"StageCraft.Entitlement.Pro.WebRemote",		"The Web Remote for phones and tablets.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Entitlement_Pro_MvrExport,		"StageCraft.Entitlement.Pro.MvrExport",		"MVR scene export.");
}
