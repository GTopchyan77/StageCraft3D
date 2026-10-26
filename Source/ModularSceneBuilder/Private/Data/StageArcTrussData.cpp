// Copyright Epic Games, Inc. All Rights Reserved.

#include "Data/StageArcTrussData.h"

#include "Actors/StageArcTrussActor.h"

#if WITH_EDITOR
#include "Misc/DataValidation.h"
#endif

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageArcTrussData)

#define LOCTEXT_NAMESPACE "StageCraftStageArcTrussData"

namespace StageArcTrussData
{
	constexpr float AbsoluteMinRadius = 50.f;
	constexpr float AbsoluteMaxRadius = 5000.f;
	constexpr float AbsoluteMinAngle = 5.f;
	constexpr float AbsoluteMaxAngle = 360.f;
}

UStageArcTrussData::UStageArcTrussData()
{
	ItemType = EStageItemType::Truss;
	ActorClass = AStageArcTrussActor::StaticClass();
	CategoryTag = StageCraftTags::Category_Truss_Curved;
	PlacementRules.GridSize = FVector(50.0, 50.0, 0.0);
}

float UStageArcTrussData::GetMinRadius() const
{
	return FMath::Clamp(FMath::Min(MinRadius, MaxRadius), StageArcTrussData::AbsoluteMinRadius, StageArcTrussData::AbsoluteMaxRadius);
}

float UStageArcTrussData::GetMaxRadius() const
{
	return FMath::Clamp(FMath::Max(MinRadius, MaxRadius), StageArcTrussData::AbsoluteMinRadius, StageArcTrussData::AbsoluteMaxRadius);
}

float UStageArcTrussData::GetDefaultRadius() const
{
	return FMath::Clamp(DefaultRadius, GetMinRadius(), GetMaxRadius());
}

float UStageArcTrussData::GetMinAngle() const
{
	return FMath::Clamp(FMath::Min(MinAngle, MaxAngle), StageArcTrussData::AbsoluteMinAngle, StageArcTrussData::AbsoluteMaxAngle);
}

float UStageArcTrussData::GetMaxAngle() const
{
	return FMath::Clamp(FMath::Max(MinAngle, MaxAngle), StageArcTrussData::AbsoluteMinAngle, StageArcTrussData::AbsoluteMaxAngle);
}

float UStageArcTrussData::GetDefaultAngle() const
{
	return FMath::Clamp(DefaultAngle, GetMinAngle(), GetMaxAngle());
}

#if WITH_EDITOR

EDataValidationResult UStageArcTrussData::IsDataValid(FDataValidationContext& Context) const
{
	EDataValidationResult Result = Super::IsDataValid(Context);
	const FText AssetName = FText::FromName(GetFName());
	if (MinRadius > MaxRadius || MinAngle > MaxAngle)
	{
		Context.AddError(FText::Format(LOCTEXT("Ranges", "{0}: a Min value is above its Max."), AssetName));
		Result = EDataValidationResult::Invalid;
	}
	const UClass* SpawnedClass = ActorClass.LoadSynchronous();
	if (SpawnedClass && !SpawnedClass->IsChildOf(AStageArcTrussActor::StaticClass()))
	{
		Context.AddError(FText::Format(LOCTEXT("NotArc", "{0}: ActorClass must be a StageArcTrussActor (or a Blueprint of it), otherwise radius and angle are ignored."), AssetName));
		Result = EDataValidationResult::Invalid;
	}
	if (!Mesh.IsNull())
	{
		Context.AddWarning(FText::Format(LOCTEXT("MeshUnused", "{0}: Mesh is ignored by curved truss; it is built from its radius and angle."), AssetName));
	}
	return Result;
}

#endif // WITH_EDITOR

#undef LOCTEXT_NAMESPACE
