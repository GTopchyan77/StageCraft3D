// Copyright Epic Games, Inc. All Rights Reserved.

#include "Actors/StagePropActor.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Components/StageSpanGeometryComponent.h"
#include "Data/StagePropData.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "People/StageFigureActors.h"
#include "People/StageFigureAssetSubsystem.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StagePropActor)

AStagePropActor::AStagePropActor()
{
	Parts = CreateDefaultSubobject<UStageSpanGeometryComponent>(TEXT("Parts"));
	Parts->SetupAttachment(MeshComponent);

	Figures = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("Figures"));
	Figures->SetupAttachment(MeshComponent);
	StageFigureActors::ConfigureFigureComponent(*Figures, EStageFigureCollision::Selectable);
}

UStagePropData* AStagePropActor::GetPropData() const
{
	return Cast<UStagePropData>(ItemData);
}

int32 AStagePropActor::GetFigureCount() const
{
	return Figures->GetInstanceCount();
}

int32 AStagePropActor::GetPartInstanceCount() const
{
	return Parts->GetInstanceCount();
}

void AStagePropActor::ApplyItemData(const UBaseItemData& Data)
{
	Super::ApplyItemData(Data);

	const UStagePropData* Prop = Cast<UStagePropData>(&Data);
	if (!Prop)
	{
		Parts->Clear();
		Figures->ClearInstances();
		return;
	}
	Parts->BuildParts(Prop->Parts);
	BuildFigures(*Prop);
	UpdateHighlight();
}

void AStagePropActor::BuildFigures(const UStagePropData& Prop)
{
	Figures->ClearInstances();
	Figures->SetCastShadow(Prop.bFiguresCastShadows);
	if (Prop.Figures.IsEmpty() || !Figures->IsRegistered())
	{
		return;
	}

	TArray<StageAudienceMath::FPerson> People;
	People.Reserve(Prop.Figures.Num());
	for (const FStageFigurePlacement& Placement : Prop.Figures)
	{
		StageAudienceMath::FPerson& Person = People.AddDefaulted_GetRef();
		Person.Transform = Placement.Transform;
		Person.Clip = Placement.Clip;
		Person.Phase = FMath::Clamp(Placement.Phase, 0.f, 1.f);
		Person.CyclesPerSecond = StageFigure::GetClipCyclesPerSecond(Placement.Clip) * FMath::Clamp(Placement.RateScale, 0.1f, 4.f);
		Person.TopColor = Placement.TopColor;
		Person.SkinTone = FMath::Clamp(Placement.SkinTone, 0.f, 1.f);
		Person.BottomShade = FMath::Clamp(Placement.BottomShade, 0.f, 1.f);
	}
	StageFigureActors::ShowPeople(*Figures, People, Prop.FigureMaterial.LoadSynchronous());
}

void AStagePropActor::UpdateHighlight()
{
	Super::UpdateHighlight();
	Parts->SetOverlayMaterial(GetHighlightOverlay());
}
