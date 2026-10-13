// Copyright Epic Games, Inc. All Rights Reserved.

#include "Components/StageSpanGeometryComponent.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Data/StageSpanData.h"
#include "Engine/CollisionProfile.h"
#include "Engine/StaticMesh.h"
#include "Interaction/StageCraftCollision.h"
#include "Materials/MaterialInterface.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageSpanGeometryComponent)

namespace StageSpanGeometry
{
	// Same as the placement ghost: above beams and haze cards, so the run being drawn is never painted over.
	constexpr int32 PreviewTranslucencySortPriority = 900;

	/** The instances of one mesh + material pair, gathered before any component is touched. */
	struct FPendingBatch
	{
		UStaticMesh* Mesh = nullptr;
		UMaterialInterface* Material = nullptr;
		TArray<FTransform> Transforms;
	};

	FPendingBatch& FindOrAddBatch(TArray<FPendingBatch>& Batches, UStaticMesh* Mesh, UMaterialInterface* Material)
	{
		for (FPendingBatch& Batch : Batches)
		{
			if (Batch.Mesh == Mesh && Batch.Material == Material)
			{
				return Batch;
			}
		}
		FPendingBatch& Batch = Batches.AddDefaulted_GetRef();
		Batch.Mesh = Mesh;
		Batch.Material = Material;
		return Batch;
	}
}

UStageSpanGeometryComponent::UStageSpanGeometryComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	SetMobility(EComponentMobility::Movable);
}

void UStageSpanGeometryComponent::SetUsage(EStageSpanGeometryUsage InUsage, UMaterialInterface* InPreviewMaterial)
{
	ensureMsgf(Batches.IsEmpty(), TEXT("%s: SetUsage after a build does not reconfigure existing parts."), *GetPathName());
	Usage = InUsage;
	PreviewMaterial = InPreviewMaterial;
}

void UStageSpanGeometryComponent::Build(const UStageSpanData* Data, const FStageSpanPathLayout& Layout)
{
	using namespace StageSpanGeometry;

	if (!IsRegistered())
	{
		return;
	}
	if (!Data || !Layout.IsValid())
	{
		Clear();
		return;
	}

	TArray<FPendingBatch> Pending;
	TArray<FTransform> PartTransforms;
	for (const FStageSpanPart& Part : Data->SegmentParts)
	{
		if (UStaticMesh* Mesh = Part.Mesh.LoadSynchronous())
		{
			StageSpanMath::MakePathSegmentInstances(Layout, Data->SegmentLength, Part.Transform, PartTransforms);
			FindOrAddBatch(Pending, Mesh, Part.Material.LoadSynchronous()).Transforms.Append(PartTransforms);
		}
	}

	// Joint and corner parts are authored relative to their joint point and placed in each joint's frame.
	TArray<FTransform> Joints;
	TArray<FTransform> Corners;
	StageSpanMath::MakePathJointFrames(Layout, Joints, Corners);
	const auto AddPartsAt = [&Pending](const TArray<FStageSpanPart>& Parts, const TArray<FTransform>& Frames)
	{
		for (const FStageSpanPart& Part : Parts)
		{
			UStaticMesh* Mesh = Frames.IsEmpty() ? nullptr : Part.Mesh.LoadSynchronous();
			if (!Mesh)
			{
				continue;
			}
			TArray<FTransform>& Transforms = FindOrAddBatch(Pending, Mesh, Part.Material.LoadSynchronous()).Transforms;
			for (const FTransform& Frame : Frames)
			{
				Transforms.Add(Part.Transform * Frame);
			}
		}
	};
	AddPartsAt(Data->JointParts, Joints);
	AddPartsAt(Data->CornerParts.IsEmpty() ? Data->JointParts : Data->CornerParts, Corners);

	for (int32 Index = 0; Index < Pending.Num(); ++Index)
	{
		UInstancedStaticMeshComponent* Batch = AcquireBatch(Index);
		Batch->ClearInstances();
		if (Batch->GetStaticMesh() != Pending[Index].Mesh)
		{
			Batch->SetStaticMesh(Pending[Index].Mesh);
		}
		ApplyMaterials(*Batch, Pending[Index].Material);
		Batch->AddInstances(Pending[Index].Transforms, /*bShouldReturnIndices*/ false, /*bWorldSpace*/ false, /*bUpdateNavigation*/ false);
	}
	TrimBatches(Pending.Num());
}

void UStageSpanGeometryComponent::Clear()
{
	TrimBatches(0);
}

void UStageSpanGeometryComponent::SetOverlayMaterial(UMaterialInterface* InOverlayMaterial)
{
	OverlayMaterial = InOverlayMaterial;
	for (UInstancedStaticMeshComponent* Batch : Batches)
	{
		if (Batch)
		{
			Batch->SetOverlayMaterial(OverlayMaterial);
		}
	}
}

int32 UStageSpanGeometryComponent::GetInstanceCount() const
{
	int32 Count = 0;
	for (const UInstancedStaticMeshComponent* Batch : Batches)
	{
		Count += Batch ? Batch->GetInstanceCount() : 0;
	}
	return Count;
}

UInstancedStaticMeshComponent* UStageSpanGeometryComponent::AcquireBatch(int32 Index)
{
	check(Index <= Batches.Num());
	if (Batches.IsValidIndex(Index) && Batches[Index])
	{
		return Batches[Index];
	}

	// Outer is the owning actor, so the actor owns the component (bounds, hiding, collision toggles and destruction include it).
	UInstancedStaticMeshComponent* Batch = NewObject<UInstancedStaticMeshComponent>(GetOwner(), NAME_None, RF_Transient);
	Batch->SetupAttachment(this);
	ConfigureBatch(*Batch);
	Batch->RegisterComponent();

	if (Batches.IsValidIndex(Index))
	{
		Batches[Index] = Batch;
	}
	else
	{
		Batches.Add(Batch);
	}
	return Batch;
}

void UStageSpanGeometryComponent::ConfigureBatch(UInstancedStaticMeshComponent& Batch) const
{
	Batch.SetMobility(EComponentMobility::Movable);
	Batch.SetCanEverAffectNavigation(false);
	Batch.SetGenerateOverlapEvents(false);
	Batch.SetOverlayMaterial(OverlayMaterial);

	if (Usage == EStageSpanGeometryUsage::Preview)
	{
		// Never hit by any trace, in particular the placement trace that positions it.
		Batch.SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Batch.SetCastShadow(false);
		Batch.SetReceivesDecals(false);
		Batch.SetAffectDynamicIndirectLighting(false);
		Batch.SetAffectDistanceFieldLighting(false);
		Batch.SetVisibleInRayTracing(false);
		Batch.bVisibleInReflectionCaptures = false;
		Batch.bVisibleInRealTimeSkyCaptures = false;
		Batch.SetTranslucentSortPriority(StageSpanGeometry::PreviewTranslucencySortPriority);
		return;
	}

	// The same responses as AModularBaseActor's mesh: selectable on the item channel, a surface for placement, never physics.
	Batch.SetCollisionProfileName(UCollisionProfile::BlockAllDynamic_ProfileName);
	Batch.SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	Batch.SetCollisionResponseToChannel(StageCraftCollision::StageItemChannel, ECR_Block);
}

void UStageSpanGeometryComponent::ApplyMaterials(UInstancedStaticMeshComponent& Batch, UMaterialInterface* PartMaterial) const
{
	UMaterialInterface* Override = Usage == EStageSpanGeometryUsage::Preview ? PreviewMaterial.Get() : PartMaterial;
	Batch.EmptyOverrideMaterials();
	if (!Override)
	{
		return;
	}
	for (int32 Slot = 0; Slot < Batch.GetNumMaterials(); ++Slot)
	{
		Batch.SetMaterial(Slot, Override);
	}
}

void UStageSpanGeometryComponent::TrimBatches(int32 Count)
{
	for (int32 Index = Batches.Num() - 1; Index >= Count; --Index)
	{
		if (UInstancedStaticMeshComponent* Batch = Batches[Index])
		{
			Batch->DestroyComponent();
		}
	}
	Batches.SetNum(FMath::Min(Count, Batches.Num()));
}
