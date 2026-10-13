// Copyright Epic Games, Inc. All Rights Reserved.

#include "Data/StageSpanData.h"

#include "Actors/StageSpanActor.h"

#if WITH_EDITOR
#include "Misc/DataValidation.h"
#endif

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageSpanData)

#define LOCTEXT_NAMESPACE "StageCraftStageSpanData"

UStageSpanData::UStageSpanData()
{
	ItemType = EStageItemType::Truss;
	ActorClass = AStageSpanActor::StaticClass();
	CategoryTag = StageCraftTags::Category_Truss_Straight;
	// The start point snaps like a truss piece; the end point follows from the segment length and the angle snap.
	PlacementRules.GridSize = FVector(50.0, 50.0, 0.0);
}

#if WITH_EDITOR

EDataValidationResult UStageSpanData::IsDataValid(FDataValidationContext& Context) const
{
	EDataValidationResult Result = Super::IsDataValid(Context);
	const FText AssetName = FText::FromName(GetFName());

	if (SegmentParts.IsEmpty())
	{
		Context.AddError(FText::Format(LOCTEXT("NoParts", "{0}: a span needs at least one Segment Part, otherwise its runs are invisible."), AssetName));
		Result = EDataValidationResult::Invalid;
	}

	const auto CheckParts = [&Context, &Result, &AssetName](const TArray<FStageSpanPart>& Parts, const FText& ListName)
	{
		for (int32 Index = 0; Index < Parts.Num(); ++Index)
		{
			if (Parts[Index].Mesh.IsNull())
			{
				Context.AddError(FText::Format(LOCTEXT("PartWithoutMesh", "{0}: {1} [{2}] has no Mesh."), AssetName, ListName, FText::AsNumber(Index)));
				Result = EDataValidationResult::Invalid;
			}
		}
	};
	CheckParts(SegmentParts, LOCTEXT("SegmentParts", "Segment Parts"));
	CheckParts(JointParts, LOCTEXT("JointParts", "Joint Parts"));

	if (MaxSegments < 1 || MaxSegments > AbsoluteMaxSegments)
	{
		Context.AddError(FText::Format(LOCTEXT("BadMaxSegments", "{0}: Max Segments must be between 1 and {1}."), AssetName, FText::AsNumber(AbsoluteMaxSegments)));
		Result = EDataValidationResult::Invalid;
	}

	const UClass* SpawnedClass = ActorClass.LoadSynchronous();
	if (SpawnedClass && !SpawnedClass->IsChildOf(AStageSpanActor::StaticClass()))
	{
		Context.AddError(FText::Format(LOCTEXT("NotASpanActor", "{0}: ActorClass must be a StageSpanActor (or a Blueprint of it), otherwise the run's length is ignored."), AssetName));
		Result = EDataValidationResult::Invalid;
	}

	if (!Mesh.IsNull())
	{
		Context.AddWarning(FText::Format(LOCTEXT("MeshUnused", "{0}: Mesh is ignored by spans; the run is built from Segment Parts and Joint Parts."), AssetName));
	}

	return Result;
}

#endif // WITH_EDITOR

#undef LOCTEXT_NAMESPACE
