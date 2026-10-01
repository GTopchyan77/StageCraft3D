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
}
