// Copyright Epic Games, Inc. All Rights Reserved.

#include "Viewport/StageGridActor.h"

#include "Components/LineBatchComponent.h"
#include "Engine/World.h"
#include "ModularSceneBuilder.h"
#include "Render/StageRenderSubsystem.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageGridActor)

AStageGridActor::AStageGridActor()
{
	PrimaryActorTick.bCanEverTick = false;
	SetCanBeDamaged(false);
	SetActorEnableCollision(false);

	Lines = CreateDefaultSubobject<ULineBatchComponent>(TEXT("Lines"));
	// The batch ticks only to age lines with a lifetime; these never expire, so it never needs to tick.
	Lines->PrimaryComponentTick.bCanEverTick = false;
	Lines->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Lines->SetCastShadow(false);
	Lines->SetMobility(EComponentMobility::Movable);
	SetRootComponent(Lines);
}

void AStageGridActor::BeginPlay()
{
	Super::BeginPlay();
	if (UStageRenderSubsystem* Render = UWorld::GetSubsystem<UStageRenderSubsystem>(GetWorld()))
	{
		Render->RegisterEditorHelper(this);
	}
}

void AStageGridActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (UStageRenderSubsystem* Render = UWorld::GetSubsystem<UStageRenderSubsystem>(GetWorld()))
	{
		Render->UnregisterEditorHelper(this);
	}
	Super::EndPlay(EndPlayReason);
}

void AStageGridActor::ShowGrid(StageViewMath::EGridPlane Plane, double PlaneOffset, double CellCm)
{
	if (!(CellCm > 0.0) || !FMath::IsFinite(CellCm))
	{
		HideGrid();
		return;
	}

	const FGridBuild Build{ Plane, PlaneOffset, CellCm };
	if (LastBuild.IsSet() && LastBuild.GetValue() == Build)
	{
		return;
	}

	StageViewMath::BuildGridLines(Plane, PlaneOffset, CellCm, HalfExtentCm, MajorEvery, ScratchLines);

	TArray<FBatchedLine> Batch;
	Batch.Reserve(ScratchLines.Num());
	for (const StageViewMath::FGridLine& Line : ScratchLines)
	{
		// Thickness 0 = one pixel at any distance or zoom; lifetime 0 = until the next rebuild.
		Batch.Emplace(Line.Start, Line.End, GetLineColor(Line.Kind), /*LifeTime*/ 0.f, /*Thickness*/ 0.f, SDPG_World);
	}

	Lines->Flush();
	Lines->DrawLines(Batch);
	SetActorHiddenInGame(false);
	LastBuild = Build;
	UE_LOG(LogStageCraft, Verbose, TEXT("Grid: %d lines, %.0f cm cells."), Batch.Num(), CellCm);
}

void AStageGridActor::HideGrid()
{
	if (!LastBuild.IsSet())
	{
		return;
	}
	Lines->Flush();
	SetActorHiddenInGame(true);
	LastBuild.Reset();
}

int32 AStageGridActor::GetLineCount() const
{
	return Lines ? Lines->BatchedLines.Num() : 0;
}

FLinearColor AStageGridActor::GetLineColor(StageViewMath::EGridLineKind Kind) const
{
	using StageViewMath::EGridLineKind;
	switch (Kind)
	{
	case EGridLineKind::Major:	return MajorColor;
	case EGridLineKind::AxisX:	return AxisXColor;
	case EGridLineKind::AxisY:	return AxisYColor;
	case EGridLineKind::AxisZ:	return AxisZColor;
	case EGridLineKind::Minor:
	default:					return MinorColor;
	}
}
