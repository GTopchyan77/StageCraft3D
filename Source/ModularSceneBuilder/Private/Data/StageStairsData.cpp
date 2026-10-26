// Copyright Epic Games, Inc. All Rights Reserved.

#include "Data/StageStairsData.h"

#include "Actors/StageStairsActor.h"

#if WITH_EDITOR
#include "Misc/DataValidation.h"
#endif

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageStairsData)

#define LOCTEXT_NAMESPACE "StageCraftStageStairsData"

namespace StageStairsData
{
	constexpr float AbsoluteMinRise = 10.f;
	constexpr float AbsoluteMaxRise = 300.f;
	constexpr float AbsoluteMinWidth = 40.f;
	constexpr float AbsoluteMaxWidth = 600.f;
}

UStageStairsData::UStageStairsData()
{
	ItemType = EStageItemType::StageElement;
	ActorClass = AStageStairsActor::StaticClass();
	CategoryTag = StageCraftTags::Category_Stage_Riser;
	PlacementRules.GridSize = FVector(10.0, 10.0, 0.0);
}

float UStageStairsData::GetMinRise() const
{
	return FMath::Clamp(FMath::Min(MinRise, MaxRise), StageStairsData::AbsoluteMinRise, StageStairsData::AbsoluteMaxRise);
}

float UStageStairsData::GetMaxRise() const
{
	return FMath::Clamp(FMath::Max(MinRise, MaxRise), StageStairsData::AbsoluteMinRise, StageStairsData::AbsoluteMaxRise);
}

float UStageStairsData::GetMinWidth() const
{
	return FMath::Clamp(FMath::Min(MinWidth, MaxWidth), StageStairsData::AbsoluteMinWidth, StageStairsData::AbsoluteMaxWidth);
}

float UStageStairsData::GetMaxWidth() const
{
	return FMath::Clamp(FMath::Max(MinWidth, MaxWidth), StageStairsData::AbsoluteMinWidth, StageStairsData::AbsoluteMaxWidth);
}

float UStageStairsData::GetDefaultRise() const
{
	return FMath::Clamp(DefaultRise, GetMinRise(), GetMaxRise());
}

float UStageStairsData::GetDefaultWidth() const
{
	return FMath::Clamp(DefaultWidth, GetMinWidth(), GetMaxWidth());
}

#if WITH_EDITOR

EDataValidationResult UStageStairsData::IsDataValid(FDataValidationContext& Context) const
{
	EDataValidationResult Result = Super::IsDataValid(Context);
	const FText AssetName = FText::FromName(GetFName());
	if (MinRise > MaxRise || MinWidth > MaxWidth)
	{
		Context.AddError(FText::Format(LOCTEXT("Ranges", "{0}: a Min value is above its Max."), AssetName));
		Result = EDataValidationResult::Invalid;
	}
	const UClass* SpawnedClass = ActorClass.LoadSynchronous();
	if (SpawnedClass && !SpawnedClass->IsChildOf(AStageStairsActor::StaticClass()))
	{
		Context.AddError(FText::Format(LOCTEXT("NotStairs", "{0}: ActorClass must be a StageStairsActor (or a Blueprint of it), otherwise rise and width are ignored."), AssetName));
		Result = EDataValidationResult::Invalid;
	}
	if (!Mesh.IsNull())
	{
		Context.AddWarning(FText::Format(LOCTEXT("MeshUnused", "{0}: Mesh is ignored by stairs; they are built from their rise and width."), AssetName));
	}
	return Result;
}

#endif // WITH_EDITOR

#undef LOCTEXT_NAMESPACE
