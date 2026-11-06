// Copyright Epic Games, Inc. All Rights Reserved.

#include "Data/StagePropData.h"

#include "Actors/StagePropActor.h"

#if WITH_EDITOR
#include "Misc/DataValidation.h"
#endif

#include UE_INLINE_GENERATED_CPP_BY_NAME(StagePropData)

#define LOCTEXT_NAMESPACE "StageCraftPropData"

// --- Prop ---

UStagePropData::UStagePropData()
{
	ItemType = EStageItemType::Prop;
	ActorClass = AStagePropActor::StaticClass();
	CategoryTag = StageCraftTags::Category_Prop;
}

#if WITH_EDITOR

EDataValidationResult UStagePropData::IsDataValid(FDataValidationContext& Context) const
{
	EDataValidationResult Result = Super::IsDataValid(Context);
	const FText AssetName = FText::FromName(GetFName());

	if (Parts.IsEmpty() && Figures.IsEmpty())
	{
		Context.AddError(FText::Format(LOCTEXT("Empty", "{0}: a prop needs at least one part or figure."), AssetName));
		Result = EDataValidationResult::Invalid;
	}
	if (!Figures.IsEmpty() && FigureMaterial.IsNull())
	{
		Context.AddError(FText::Format(LOCTEXT("NoFigureMaterial", "{0}: Figure Material is required for figures (M_StageFigure)."), AssetName));
		Result = EDataValidationResult::Invalid;
	}
	for (const FStageSpanPart& Part : Parts)
	{
		if (Part.Mesh.IsNull())
		{
			Context.AddError(FText::Format(LOCTEXT("PartMesh", "{0}: every part needs a mesh."), AssetName));
			Result = EDataValidationResult::Invalid;
			break;
		}
	}
	const UClass* SpawnedClass = ActorClass.LoadSynchronous();
	if (SpawnedClass && !SpawnedClass->IsChildOf(AStagePropActor::StaticClass()))
	{
		Context.AddError(FText::Format(LOCTEXT("NotAProp", "{0}: ActorClass must be a StagePropActor, otherwise the parts and figures are ignored."), AssetName));
		Result = EDataValidationResult::Invalid;
	}
	if (!Mesh.IsNull())
	{
		Context.AddWarning(FText::Format(LOCTEXT("PropMeshUnused", "{0}: Mesh is ignored by props; use Parts."), AssetName));
	}
	return Result;
}

#endif // WITH_EDITOR

#undef LOCTEXT_NAMESPACE
