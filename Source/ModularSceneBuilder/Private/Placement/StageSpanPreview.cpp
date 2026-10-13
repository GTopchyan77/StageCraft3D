// Copyright Epic Games, Inc. All Rights Reserved.

#include "Placement/StageSpanPreview.h"

#include "Components/StageSpanGeometryComponent.h"
#include "Data/StageSpanData.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "ModularSceneBuilder.h"
#include "UObject/ConstructorHelpers.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageSpanPreview)

namespace StageSpanPreview
{
	const FName GhostColorParameter(TEXT("GhostColor"));
}

AStageSpanPreview::AStageSpanPreview()
{
	PrimaryActorTick.bCanEverTick = false;

	static ConstructorHelpers::FObjectFinder<UMaterialInterface> GhostMaterialFinder(TEXT("/Game/StageCraft/Placement/M_PlacementGhost.M_PlacementGhost"));
	GhostMaterial = GhostMaterialFinder.Object;

	Geometry = CreateDefaultSubobject<UStageSpanGeometryComponent>(TEXT("Geometry"));
	SetRootComponent(Geometry);

	SetActorEnableCollision(false);
	SetActorHiddenInGame(true);
}

void AStageSpanPreview::BeginPlay()
{
	Super::BeginPlay();

	if (GhostMaterial)
	{
		GhostMaterialInstance = UMaterialInstanceDynamic::Create(GhostMaterial, this);
	}
	else
	{
		UE_LOG(LogStageCraft, Warning, TEXT("%s: GhostMaterial missing; the run preview uses the parts' own materials."), *GetName());
	}
	// Before any build, so every part is drawn with the ghost material.
	Geometry->SetUsage(EStageSpanGeometryUsage::Preview, GhostMaterialInstance);
}

void AStageSpanPreview::SetItem(const UStageSpanData* InItem)
{
	if (Item == InItem)
	{
		return;
	}
	Item = InItem;
	BuiltLayout = FStageSpanLayout();
	Geometry->Clear();
	HidePreview();
}

void AStageSpanPreview::ShowRun(const FTransform& RunTransform, const FStageSpanLayout& Layout, EStagePlacementPreviewState State)
{
	if (!ensureMsgf(State != EStagePlacementPreviewState::Hidden, TEXT("ShowRun needs a visible state; call HidePreview instead.")) || !Item || !Layout.IsValid())
	{
		HidePreview();
		return;
	}

	if (!(Layout == BuiltLayout))
	{
		Geometry->Build(Item, Layout);
		BuiltLayout = Layout;
	}

	SetActorTransform(RunTransform);
	if (GhostMaterialInstance)
	{
		GhostMaterialInstance->SetVectorParameterValue(StageSpanPreview::GhostColorParameter, State == EStagePlacementPreviewState::Refused ? RefusedColor : ValidColor);
	}
	SetActorHiddenInGame(false);
}

void AStageSpanPreview::HidePreview()
{
	SetActorHiddenInGame(true);
}
