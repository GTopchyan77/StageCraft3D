// Copyright Epic Games, Inc. All Rights Reserved.

#include "Data/StageTrussData.h"

#include "Actors/StageTrussActor.h"

#if WITH_EDITOR
#include "Misc/DataValidation.h"
#endif

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageTrussData)

#define LOCTEXT_NAMESPACE "StageCraftStageTrussData"

UStageTrussData::UStageTrussData()
{
	ItemType = EStageItemType::Truss;
	ActorClass = AStageTrussActor::StaticClass();
	CategoryTag = StageCraftTags::Category_Truss_Straight;
	PlacementRules.PlacementMode = EStageItemPlacementMode::Single;
	PlacementRules.GridSize = FVector(50.0, 50.0, 0.0);
	Specs.WeightKg = 14.f;
}

#if WITH_EDITOR

EDataValidationResult UStageTrussData::IsDataValid(FDataValidationContext& Context) const
{
	EDataValidationResult Result = Super::IsDataValid(Context);
	const FText AssetName = FText::FromName(GetFName());

	if (PieceKind == EStageTrussPieceKind::Straight && Length <= 0.f)
	{
		Context.AddError(FText::Format(LOCTEXT("NoLength", "{0}: Straight truss needs a Length."), AssetName));
		Result = EDataValidationResult::Invalid;
	}

	for (const FStageRiggingPoint& Point : RiggingPoints)
	{
		if (Point.SafeWorkingLoadKg > MaxPointLoadKg && MaxPointLoadKg > 0.f)
		{
			Context.AddWarning(FText::Format(LOCTEXT("PointOverSpan", "{0}: Rigging point '{1}' allows more load than the piece's MaxPointLoadKg."), AssetName, FText::FromName(Point.Name)));
		}
	}

	return Result;
}

#endif // WITH_EDITOR

#undef LOCTEXT_NAMESPACE
