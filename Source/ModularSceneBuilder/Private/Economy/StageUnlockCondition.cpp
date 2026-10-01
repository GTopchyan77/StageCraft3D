// Copyright Epic Games, Inc. All Rights Reserved.

#include "Economy/StageUnlockCondition.h"

#include "Subsystems/StageProfileSubsystem.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageUnlockCondition)

#define LOCTEXT_NAMESPACE "StageCraftUnlockCondition"

bool UStageUnlockCondition::IsMet_Implementation(const UStageProfileSubsystem* Profile, FText& OutReason) const
{
	// An abstract condition that nobody implemented must not unlock anything.
	OutReason = LOCTEXT("NotImplemented", "Unavailable.");
	return false;
}

bool UStageUnlockCondition_Entitlements::IsMet_Implementation(const UStageProfileSubsystem* Profile, FText& OutReason) const
{
	if (!Profile)
	{
		OutReason = LOCTEXT("NoProfile", "No player profile.");
		return false;
	}

	for (const FGameplayTag& Required : RequiredEntitlements)
	{
		if (!Profile->HasEntitlement(Required))
		{
			OutReason = FText::Format(LOCTEXT("MissingEntitlement", "Requires {0}."), FText::FromName(Required.GetTagName()));
			return false;
		}
	}
	return true;
}

bool UStageUnlockCondition_ProfileLevel::IsMet_Implementation(const UStageProfileSubsystem* Profile, FText& OutReason) const
{
	if (!Profile || Profile->GetPlayerLevel() < MinLevel)
	{
		OutReason = FText::Format(LOCTEXT("LevelTooLow", "Requires level {0}."), FText::AsNumber(MinLevel));
		return false;
	}
	return true;
}

#undef LOCTEXT_NAMESPACE
