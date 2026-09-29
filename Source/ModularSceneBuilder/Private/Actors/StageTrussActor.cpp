// Copyright Epic Games, Inc. All Rights Reserved.

#include "Actors/StageTrussActor.h"

#include "Data/StageTrussData.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageTrussActor)

#define LOCTEXT_NAMESPACE "StageCraftStageTrussActor"

UStageTrussData* AStageTrussActor::GetTrussData() const
{
	return Cast<UStageTrussData>(ItemData);
}

TArray<FTransform> AStageTrussActor::GetRiggingPointWorldTransforms() const
{
	TArray<FTransform> Result;
	if (const UStageTrussData* Truss = GetTrussData())
	{
		const FTransform& ActorTransform = GetActorTransform();
		Result.Reserve(Truss->RiggingPoints.Num());
		for (const FStageRiggingPoint& Point : Truss->RiggingPoints)
		{
			Result.Add(Point.LocalTransform * ActorTransform);
		}
	}
	return Result;
}

TArray<FTransform> AStageTrussActor::GetConnectorWorldTransforms() const
{
	TArray<FTransform> Result;
	if (const UStageTrussData* Truss = GetTrussData())
	{
		const FTransform& ActorTransform = GetActorTransform();
		Result.Reserve(Truss->Connectors.Num());
		for (const FStageTrussConnector& Connector : Truss->Connectors)
		{
			Result.Add(Connector.LocalTransform * ActorTransform);
		}
	}
	return Result;
}

void AStageTrussActor::GatherParameterSections(TArray<FStageParameterSection>& OutSections) const
{
	Super::GatherParameterSections(OutSections);

	const UStageTrussData* Truss = GetTrussData();
	if (!Truss)
	{
		return;
	}

	FStageParameterSection& Section = OutSections.Emplace_GetRef(StageCraftTags::FeatureGroup_Rigging, LOCTEXT("RiggingSection", "Rigging"));

	if (Truss->PieceKind != EStageTrussPieceKind::Hoist)
	{
		Section.Add(StageCraftTags::Param_Rigging_Profile, LOCTEXT("Profile", "Profile"),
			FStageParameterValue::MakeText(FText::Format(LOCTEXT("ProfileFormat", "{0}, {1} cm"),
				StaticEnum<EStageTrussProfile>()->GetDisplayValueAsText(Truss->Profile), FText::AsNumber(Truss->ProfileWidth)))).ReadOnly();
		Section.Add(StageCraftTags::Param_Rigging_Length, LOCTEXT("Length", "Length"), FStageParameterValue::MakeFloat(Truss->Length))
			.Display(1.0, LOCTEXT("Cm", "cm")).ReadOnly();
	}

	Section.Add(StageCraftTags::Param_Rigging_MaxPointLoad,
		Truss->PieceKind == EStageTrussPieceKind::Hoist ? LOCTEXT("Capacity", "Rated Capacity") : LOCTEXT("MaxPointLoad", "Max Point Load"),
		FStageParameterValue::MakeFloat(Truss->MaxPointLoadKg)).Display(1.0, LOCTEXT("Kg", "kg")).ReadOnly();
	Section.Add(StageCraftTags::Param_Rigging_Points, LOCTEXT("Points", "Rigging Points"), FStageParameterValue::MakeInteger(Truss->RiggingPoints.Num())).ReadOnly();
}

#undef LOCTEXT_NAMESPACE
