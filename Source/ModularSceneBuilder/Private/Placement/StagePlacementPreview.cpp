// Copyright Epic Games, Inc. All Rights Reserved.

#include "Placement/StagePlacementPreview.h"

#include "Components/DecalComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Data/BaseItemData.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "ModularSceneBuilder.h"
#include "Placement/StageSnapGuidesComponent.h"
#include "UObject/ConstructorHelpers.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StagePlacementPreview)

namespace StagePlacementPreview
{
	const FName GhostColorParameter(TEXT("GhostColor"));
	const FName MarkerColorParameter(TEXT("MarkerColor"));

	// Decal projection depth either side of the surface; deep enough for uneven stage decks, shallow
	// enough not to paint the item below on a stack.
	constexpr double MarkerProjectionHalfDepth = 20.0;

	// Above beams and haze cards, like the gizmo, so the ghost is never painted over by scene translucency.
	constexpr int32 GhostTranslucencySortPriority = 900;
}

AStagePlacementPreview::AStagePlacementPreview()
{
	PrimaryActorTick.bCanEverTick = false;

	static ConstructorHelpers::FObjectFinder<UMaterialInterface> GhostMaterialFinder(TEXT("/Game/StageCraft/Placement/M_PlacementGhost.M_PlacementGhost"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> MarkerMaterialFinder(TEXT("/Game/StageCraft/Placement/M_PlacementMarker.M_PlacementMarker"));
	GhostMaterial = GhostMaterialFinder.Object;
	MarkerMaterial = MarkerMaterialFinder.Object;

	PreviewRoot = CreateDefaultSubobject<USceneComponent>(TEXT("PreviewRoot"));
	PreviewRoot->SetMobility(EComponentMobility::Movable);
	SetRootComponent(PreviewRoot);

	GhostMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("GhostMesh"));
	GhostMesh->SetupAttachment(PreviewRoot);
	GhostMesh->SetMobility(EComponentMobility::Movable);
	// Never hit by any trace, in particular the placement trace that positions it.
	GhostMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	GhostMesh->SetGenerateOverlapEvents(false);
	GhostMesh->SetCastShadow(false);
	GhostMesh->SetReceivesDecals(false);
	GhostMesh->SetAffectDynamicIndirectLighting(false);
	GhostMesh->SetAffectDistanceFieldLighting(false);
	GhostMesh->SetVisibleInRayTracing(false);
	GhostMesh->bVisibleInReflectionCaptures = false;
	GhostMesh->bVisibleInRealTimeSkyCaptures = false;
	GhostMesh->SetTranslucentSortPriority(StagePlacementPreview::GhostTranslucencySortPriority);

	// Positioned in world space by ShowAt, independently of the ghost's rotation.
	LandingMarker = CreateDefaultSubobject<UDecalComponent>(TEXT("LandingMarker"));
	LandingMarker->SetupAttachment(PreviewRoot);
	LandingMarker->SetUsingAbsoluteLocation(true);
	LandingMarker->SetUsingAbsoluteRotation(true);
	LandingMarker->SetUsingAbsoluteScale(true);

	SnapGuidesComponent = CreateDefaultSubobject<UStageSnapGuidesComponent>(TEXT("SnapGuides"));
	SnapGuidesComponent->SetupAttachment(PreviewRoot);

	SetActorEnableCollision(false);
	SetActorHiddenInGame(true);
}

void AStagePlacementPreview::BeginPlay()
{
	Super::BeginPlay();

	if (GhostMaterial)
	{
		GhostMaterialInstance = UMaterialInstanceDynamic::Create(GhostMaterial, this);
	}
	else
	{
		UE_LOG(LogStageCraft, Warning, TEXT("%s: GhostMaterial missing; the ghost uses the item's own materials."), *GetName());
	}

	if (MarkerMaterial)
	{
		MarkerMaterialInstance = UMaterialInstanceDynamic::Create(MarkerMaterial, this);
		LandingMarker->SetDecalMaterial(MarkerMaterialInstance);
	}
	else
	{
		UE_LOG(LogStageCraft, Warning, TEXT("%s: MarkerMaterial missing; the landing marker is disabled."), *GetName());
		LandingMarker->SetVisibility(false);
	}
}

void AStagePlacementPreview::SetItem(const UBaseItemData* Item)
{
	// Normally resident: the item's Game bundle streams in before it is armed. The fallback load is once per arm, not per move.
	UStaticMesh* Mesh = Item ? Item->Mesh.LoadSynchronous() : nullptr;
	GhostMesh->SetStaticMesh(Mesh);
	GhostMesh->SetVisibility(Mesh != nullptr);
	ApplyGhostMaterials();
}

void AStagePlacementPreview::ShowAt(const FTransform& ItemTransform, const FVector& LandingPoint, const FVector& SurfaceNormal, EStagePlacementPreviewState State, TConstArrayView<FStageSnapGuide> SnapGuides)
{
	if (!ensureMsgf(State != EStagePlacementPreviewState::Hidden, TEXT("ShowAt needs a visible state; call HidePreview instead.")))
	{
		HidePreview();
		return;
	}

	SetActorTransform(ItemTransform);

	// A decal projects along its local X axis: point it into the surface.
	const FVector Normal = SurfaceNormal.GetSafeNormal(UE_SMALL_NUMBER, FVector::UpVector);
	LandingMarker->SetWorldLocationAndRotation(LandingPoint, FRotationMatrix::MakeFromX(-Normal).Rotator());
	LandingMarker->DecalSize = ComputeMarkerSize();
	LandingMarker->MarkRenderStateDirty();

	ApplyColor(State);
	SnapGuidesComponent->ShowGuides(SnapGuides);
	SetActorHiddenInGame(false);
}

void AStagePlacementPreview::HidePreview()
{
	SetActorHiddenInGame(true);
}

void AStagePlacementPreview::ApplyGhostMaterials()
{
	if (!GhostMaterialInstance)
	{
		return;
	}

	for (int32 Slot = 0; Slot < GhostMesh->GetNumMaterials(); ++Slot)
	{
		GhostMesh->SetMaterial(Slot, GhostMaterialInstance);
	}
}

void AStagePlacementPreview::ApplyColor(EStagePlacementPreviewState State)
{
	const FLinearColor& Color = State == EStagePlacementPreviewState::Refused ? RefusedColor : ValidColor;
	if (GhostMaterialInstance)
	{
		GhostMaterialInstance->SetVectorParameterValue(StagePlacementPreview::GhostColorParameter, Color);
	}
	if (MarkerMaterialInstance)
	{
		MarkerMaterialInstance->SetVectorParameterValue(StagePlacementPreview::MarkerColorParameter, Color);
	}
}

FVector AStagePlacementPreview::ComputeMarkerSize() const
{
	// DecalSize is a half-extent: X = projection depth, Y/Z = footprint across the surface.
	const UStaticMesh* Mesh = GhostMesh->GetStaticMesh();
	const FVector Extent = Mesh ? Mesh->GetBounds().BoxExtent : FVector::ZeroVector;
	const double HalfSize = FMath::Max3(Extent.X, Extent.Y, MinMarkerHalfSize);
	return FVector(StagePlacementPreview::MarkerProjectionHalfDepth, HalfSize, HalfSize);
}

int32 AStagePlacementPreview::GetVisibleSnapGuideCount() const
{
	return IsPreviewVisible() ? SnapGuidesComponent->GetVisibleGuideCount() : 0;
}
