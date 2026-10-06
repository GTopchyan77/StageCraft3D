// Copyright Epic Games, Inc. All Rights Reserved.

#include "Placement/StageSnapGuidesComponent.h"

#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "UObject/ConstructorHelpers.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageSnapGuidesComponent)

namespace StageSnapGuides
{
	// Engine BasicShapes cube: 100 cm, pivot at the centre.
	constexpr double CubeSize = 100.0;
	constexpr double MinLength = 1.0;
	const FName ColorParameter(TEXT("GizmoColor"));
	// Above the gizmo handles (1000), so a guide crossing a handle is still readable.
	constexpr int32 TranslucencySortPriority = 1001;
}

UStageSnapGuidesComponent::UStageSnapGuidesComponent()
{
	PrimaryComponentTick.bCanEverTick = false;

	static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeMesh(TEXT("/Engine/BasicShapes/Cube.Cube"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> OnTopMaterial(TEXT("/Game/StageCraft/Gizmo/M_GizmoHandle.M_GizmoHandle"));
	LineMesh = CubeMesh.Object;
	LineMaterial = OnTopMaterial.Object;
}

void UStageSnapGuidesComponent::ShowGuides(TConstArrayView<FStageSnapGuide> Guides)
{
	const int32 Count = FMath::Min(Guides.Num(), MaxGuides);
	for (int32 Index = 0; Index < Count; ++Index)
	{
		const FStageSnapGuide& Guide = Guides[Index];
		UStaticMeshComponent* Line = GetOrCreateLine(Index);
		if (!Line)
		{
			continue;
		}

		const FVector Direction = Guide.End - Guide.Start;
		const double Length = FMath::Max(Direction.Size(), StageSnapGuides::MinLength);
		const double Thickness = FMath::Max(Guide.Thickness, 0.1);
		const FRotator Rotation = Direction.IsNearlyZero() ? FRotator::ZeroRotator : Direction.Rotation();

		Line->SetWorldLocationAndRotation(0.5 * (Guide.Start + Guide.End), Rotation);
		Line->SetWorldScale3D(FVector(Length, Thickness, Thickness) / StageSnapGuides::CubeSize);
		Line->SetVisibility(true);
	}

	for (int32 Index = Count; Index < Lines.Num(); ++Index)
	{
		Lines[Index]->SetVisibility(false);
	}
}

void UStageSnapGuidesComponent::HideGuides()
{
	for (UStaticMeshComponent* Line : Lines)
	{
		Line->SetVisibility(false);
	}
}

UStaticMeshComponent* UStageSnapGuidesComponent::GetOrCreateLine(int32 Index)
{
	if (Lines.IsValidIndex(Index))
	{
		return Lines[Index];
	}

	AActor* Owner = GetOwner();
	if (!Owner || !LineMesh || !ensure(Index == Lines.Num()))
	{
		return nullptr;
	}

	if (!LineMaterialInstance && LineMaterial)
	{
		LineMaterialInstance = UMaterialInstanceDynamic::Create(LineMaterial, this);
		LineMaterialInstance->SetVectorParameterValue(StageSnapGuides::ColorParameter, GuideColor);
	}

	UStaticMeshComponent* Line = NewObject<UStaticMeshComponent>(Owner, *FString::Printf(TEXT("SnapGuide%d"), Index), RF_Transient);
	Line->SetupAttachment(this);
	// World-space lines: they must not follow the gizmo or preview they belong to.
	Line->SetUsingAbsoluteLocation(true);
	Line->SetUsingAbsoluteRotation(true);
	Line->SetUsingAbsoluteScale(true);
	Line->SetMobility(EComponentMobility::Movable);
	Line->SetStaticMesh(LineMesh);
	if (LineMaterialInstance)
	{
		Line->SetMaterial(0, LineMaterialInstance);
	}

	// An overlay, not scene content: no collision (traces never hit it) and no lighting contribution.
	Line->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Line->SetGenerateOverlapEvents(false);
	Line->SetCastShadow(false);
	Line->SetReceivesDecals(false);
	Line->SetAffectDynamicIndirectLighting(false);
	Line->SetAffectDistanceFieldLighting(false);
	Line->SetVisibleInRayTracing(false);
	Line->bVisibleInReflectionCaptures = false;
	Line->bVisibleInRealTimeSkyCaptures = false;
	Line->SetTranslucentSortPriority(StageSnapGuides::TranslucencySortPriority);
	Line->SetVisibility(false);
	Line->RegisterComponent();

	Lines.Add(Line);
	return Line;
}

int32 UStageSnapGuidesComponent::GetVisibleGuideCount() const
{
	int32 Count = 0;
	for (const UStaticMeshComponent* Line : Lines)
	{
		Count += Line->IsVisible() ? 1 : 0;
	}
	return Count;
}
