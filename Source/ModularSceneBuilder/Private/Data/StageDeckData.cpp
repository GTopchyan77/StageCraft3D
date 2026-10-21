// Copyright Epic Games, Inc. All Rights Reserved.

#include "Data/StageDeckData.h"

#include "Actors/StageDeckActor.h"

#if WITH_EDITOR
#include "Misc/DataValidation.h"
#endif

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageDeckData)

#define LOCTEXT_NAMESPACE "StageCraftStageDeckData"

UStageDeckData::UStageDeckData()
{
	ItemType = EStageItemType::StageElement;
	ActorClass = AStageDeckActor::StaticClass();
	CategoryTag = StageCraftTags::Category_Stage_Deck;
	PlacementRules.GridSize = FVector(50.0, 50.0, 0.0);
}

float UStageDeckData::GetMinHeight() const
{
	return FMath::Clamp(FMath::Min(MinHeight, MaxHeight), AbsoluteMinHeight, AbsoluteMaxHeight);
}

float UStageDeckData::GetMaxHeight() const
{
	return FMath::Clamp(FMath::Max(MinHeight, MaxHeight), AbsoluteMinHeight, AbsoluteMaxHeight);
}

float UStageDeckData::GetDefaultHeight() const
{
	return FMath::Clamp(DefaultHeight, GetMinHeight(), GetMaxHeight());
}

double UStageDeckData::GetPanelArea() const
{
	const double Area = FMath::Abs(PanelSize.X * PanelSize.Y);
	return FMath::IsFinite(Area) ? FMath::Max(1.0, Area) : 1.0;
}

#if WITH_EDITOR

EDataValidationResult UStageDeckData::IsDataValid(FDataValidationContext& Context) const
{
	EDataValidationResult Result = Super::IsDataValid(Context);
	const FText AssetName = FText::FromName(GetFName());

	if (MinHeight > MaxHeight)
	{
		Context.AddError(FText::Format(LOCTEXT("HeightRange", "{0}: Min Height is above Max Height."), AssetName));
		Result = EDataValidationResult::Invalid;
	}
	if (DefaultHeight < MinHeight || DefaultHeight > MaxHeight)
	{
		Context.AddWarning(FText::Format(LOCTEXT("DefaultHeight", "{0}: Default Height is outside Min..Max Height and will be clamped."), AssetName));
	}
	if (PanelSize.X <= 0.0 || PanelSize.Y <= 0.0 || DefaultSize.X < 50.0 || DefaultSize.Y < 50.0)
	{
		Context.AddError(FText::Format(LOCTEXT("Sizes", "{0}: Panel Size must be positive and Default Size at least 50 x 50 cm."), AssetName));
		Result = EDataValidationResult::Invalid;
	}

	const UClass* SpawnedClass = ActorClass.LoadSynchronous();
	if (SpawnedClass && !SpawnedClass->IsChildOf(AStageDeckActor::StaticClass()))
	{
		Context.AddError(FText::Format(LOCTEXT("NotADeckActor", "{0}: ActorClass must be a StageDeckActor (or a Blueprint of it), otherwise the outline and height are ignored."), AssetName));
		Result = EDataValidationResult::Invalid;
	}
	if (!Mesh.IsNull())
	{
		Context.AddWarning(FText::Format(LOCTEXT("MeshUnused", "{0}: Mesh is ignored by decks; the deck is built from its outline."), AssetName));
	}
	return Result;
}

#endif // WITH_EDITOR

#undef LOCTEXT_NAMESPACE
