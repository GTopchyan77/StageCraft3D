// Copyright Epic Games, Inc. All Rights Reserved.

#include "Viewport/StageGridActor.h"

#include "Engine/World.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "ModularSceneBuilder.h"
#include "ProceduralMeshComponent.h"
#include "Render/StageRenderSubsystem.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageGridActor)

namespace StageGridParameters
{
	const FName CellCm(TEXT("CellCm"));
	const FName MajorEvery(TEXT("MajorEvery"));
	const FName AxisU(TEXT("AxisU"));
	const FName AxisV(TEXT("AxisV"));
	const FName MinorColor(TEXT("MinorColor"));
	const FName MajorColor(TEXT("MajorColor"));
	const FName ColorU(TEXT("ColorU"));
	const FName ColorV(TEXT("ColorV"));
}

AStageGridActor::AStageGridActor()
{
	PrimaryActorTick.bCanEverTick = false;
	SetCanBeDamaged(false);
	SetActorEnableCollision(false);

	Surface = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("Surface"));
	Surface->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Surface->SetCastShadow(false);
	Surface->bAffectDynamicIndirectLighting = false;
	Surface->bAffectDistanceFieldLighting = false;
	Surface->bVisibleInRayTracing = false;
	Surface->SetMobility(EComponentMobility::Movable);
	SetRootComponent(Surface);
	SetActorHiddenInGame(true);
}

void AStageGridActor::BeginPlay()
{
	Super::BeginPlay();

	// One quad in the actor's XY plane; ShowGrid turns the actor so X and Y become the grid plane's U and V.
	const double Extent = HalfExtentCm;
	const TArray<FVector> Vertices = { FVector(-Extent, -Extent, 0.0), FVector(Extent, -Extent, 0.0), FVector(Extent, Extent, 0.0), FVector(-Extent, Extent, 0.0) };
	const TArray<int32> Triangles = { 0, 2, 1, 0, 3, 2 };
	const TArray<FVector> Normals = { FVector::UpVector, FVector::UpVector, FVector::UpVector, FVector::UpVector };
	Surface->CreateMeshSection_LinearColor(0, Vertices, Triangles, Normals, {}, {}, {}, /*bCreateCollision*/ false);

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

bool AStageGridActor::EnsureMaterial()
{
	if (GridMaterialInstance)
	{
		return true;
	}
	UMaterialInterface* Material = GridMaterial.LoadSynchronous();
	GridMaterialInstance = Material ? UMaterialInstanceDynamic::Create(Material, this) : nullptr;
	if (!GridMaterialInstance)
	{
		if (!bWarnedMissingMaterial)
		{
			UE_LOG(LogStageCraft, Warning, TEXT("Grid: hidden, the material '%s' did not load (DefaultGame.ini GridMaterial)."), *GridMaterial.ToString());
			bWarnedMissingMaterial = true;
		}
		return false;
	}
	Surface->SetMaterial(0, GridMaterialInstance);
	GridMaterialInstance->SetVectorParameterValue(StageGridParameters::MinorColor, MinorColor);
	GridMaterialInstance->SetVectorParameterValue(StageGridParameters::MajorColor, MajorColor);
	GridMaterialInstance->SetScalarParameterValue(StageGridParameters::MajorEvery, static_cast<float>(FMath::Max(MajorEvery, 0)));
	return true;
}

void AStageGridActor::ShowGrid(StageViewMath::EGridPlane Plane, double PlaneOffset, double CellCm)
{
	StageViewMath::FGridSurface GridSurface;
	if (!(CellCm > 0.0) || !FMath::IsFinite(CellCm) || !StageViewMath::GetGridSurface(Plane, PlaneOffset, GridSurface))
	{
		HideGrid();
		return;
	}

	const FGridBuild Build{ Plane, PlaneOffset, CellCm };
	if (LastBuild.IsSet() && LastBuild.GetValue() == Build)
	{
		return;
	}
	if (!EnsureMaterial())
	{
		HideGrid();
		return;
	}

	SetActorLocationAndRotation(GridSurface.Origin, FRotationMatrix::MakeFromXY(GridSurface.AxisU, GridSurface.AxisV).Rotator());
	GridMaterialInstance->SetScalarParameterValue(StageGridParameters::CellCm, static_cast<float>(CellCm));
	GridMaterialInstance->SetVectorParameterValue(StageGridParameters::AxisU, FLinearColor(GridSurface.AxisU));
	GridMaterialInstance->SetVectorParameterValue(StageGridParameters::AxisV, FLinearColor(GridSurface.AxisV));
	GridMaterialInstance->SetVectorParameterValue(StageGridParameters::ColorU, GetLineColor(GridSurface.AlongU));
	GridMaterialInstance->SetVectorParameterValue(StageGridParameters::ColorV, GetLineColor(GridSurface.AlongV));
	SetActorHiddenInGame(false);
	LastBuild = Build;
	UE_LOG(LogStageCraft, Verbose, TEXT("Grid: %.0f cm cells at %s."), CellCm, *GridSurface.Origin.ToCompactString());
}

void AStageGridActor::HideGrid()
{
	if (!LastBuild.IsSet())
	{
		return;
	}
	SetActorHiddenInGame(true);
	LastBuild.Reset();
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
