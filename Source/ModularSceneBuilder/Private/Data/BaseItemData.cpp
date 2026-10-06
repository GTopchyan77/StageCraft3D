// Copyright Epic Games, Inc. All Rights Reserved.

#include "Data/BaseItemData.h"

#include "Actors/ModularBaseActor.h"
#include "Economy/StageEconomyTypes.h"

#if WITH_EDITOR
#include "Misc/DataValidation.h"
#endif

#include UE_INLINE_GENERATED_CPP_BY_NAME(BaseItemData)

#define LOCTEXT_NAMESPACE "StageCraftBaseItemData"

const FPrimaryAssetType UBaseItemData::StageItemAssetType(TEXT("StageItem"));
const FName UBaseItemData::UIBundle(TEXT("UI"));
const FName UBaseItemData::GameBundle(TEXT("Game"));

UBaseItemData::UBaseItemData()
	: ActorClass(AModularBaseActor::StaticClass())
{
}

FPrimaryAssetId UBaseItemData::GetPrimaryAssetId() const
{
	// All subclasses share one type so the catalog is a single Asset Manager query.
	return FPrimaryAssetId(StageItemAssetType, GetFName());
}

#if WITH_EDITOR

EDataValidationResult UBaseItemData::IsDataValid(FDataValidationContext& Context) const
{
	EDataValidationResult Result = Super::IsDataValid(Context);

	if (ActorClass.IsNull())
	{
		Context.AddError(FText::Format(LOCTEXT("MissingActorClass", "{0}: ActorClass must be set, otherwise the item cannot be spawned."), FText::FromName(GetFName())));
		Result = EDataValidationResult::Invalid;
	}
	else if (Mesh.IsNull() && ActorClass.ToSoftObjectPath() == FSoftObjectPath(AModularBaseActor::StaticClass()))
	{
		// The base actor has no visuals of its own, so without a Mesh the item would spawn invisible.
		Context.AddError(FText::Format(LOCTEXT("NothingToRender", "{0}: Uses the plain ModularBaseActor but has no Mesh; set a Mesh or a Blueprint ActorClass."), FText::FromName(GetFName())));
		Result = EDataValidationResult::Invalid;
	}

	if (DisplayName.IsEmpty())
	{
		Context.AddWarning(FText::Format(LOCTEXT("MissingDisplayName", "{0}: DisplayName is empty; the catalog will show a blank label."), FText::FromName(GetFName())));
	}

	if (!CategoryTag.IsValid())
	{
		Context.AddWarning(FText::Format(LOCTEXT("MissingCategory", "{0}: CategoryTag is not set; the item will not appear under any catalog tab."), FText::FromName(GetFName())));
	}

	// A gate with a non-entitlement tag could never be owned, so the item would be locked forever.
	if (RequiredEntitlement.IsValid() && !RequiredEntitlement.MatchesTag(StageCraftTags::Entitlement))
	{
		Context.AddError(FText::Format(LOCTEXT("BadRequiredEntitlement", "{0}: RequiredEntitlement must be a StageCraft.Entitlement tag."), FText::FromName(GetFName())));
		Result = EDataValidationResult::Invalid;
	}
	for (const TPair<FGameplayTag, FGameplayTag>& Gate : ParameterEntitlements)
	{
		if (!Gate.Key.IsValid() || !Gate.Value.MatchesTag(StageCraftTags::Entitlement))
		{
			Context.AddError(FText::Format(LOCTEXT("BadParameterEntitlement", "{0}: each ParameterEntitlements entry needs a parameter tag and a StageCraft.Entitlement tag."), FText::FromName(GetFName())));
			Result = EDataValidationResult::Invalid;
			break;
		}
	}

	return Result == EDataValidationResult::NotValidated ? EDataValidationResult::Valid : Result;
}

#endif // WITH_EDITOR

#undef LOCTEXT_NAMESPACE
