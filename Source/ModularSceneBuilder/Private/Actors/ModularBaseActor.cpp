// Copyright Epic Games, Inc. All Rights Reserved.

#include "Actors/ModularBaseActor.h"

#include "Components/StaticMeshComponent.h"
#include "Data/BaseItemData.h"
#include "Engine/CollisionProfile.h"
#include "Engine/StaticMesh.h"
#include "Interaction/StageCraftCollision.h"
#include "Materials/MaterialInterface.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(ModularBaseActor)

AModularBaseActor::AModularBaseActor()
{
	PrimaryActorTick.bCanEverTick = false;

	MeshComponent = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("MeshComponent"));
	SetRootComponent(MeshComponent);

	// Placed items are moved by the gizmo and hit by cursor traces, but never simulate physics or need overlap events.
	MeshComponent->SetMobility(EComponentMobility::Movable);
	MeshComponent->SetCollisionProfileName(UCollisionProfile::BlockAllDynamic_ProfileName);
	MeshComponent->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	MeshComponent->SetCollisionResponseToChannel(StageCraftCollision::StageItemChannel, ECR_Block);
	MeshComponent->SetGenerateOverlapEvents(false);
}

void AModularBaseActor::InitializeFromItemData(UBaseItemData* InItemData)
{
	ItemData = InItemData;

	if (ItemData)
	{
		ApplyItemData(*ItemData);
		BP_OnItemDataApplied(ItemData);
	}
}

void AModularBaseActor::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);

	// Covers level-placed instances and ItemData edits in the details panel.
	if (ItemData)
	{
		ApplyItemData(*ItemData);
	}
}

void AModularBaseActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// Tells selection listeners to let go when a selected actor is deleted.
	if (bIsSelected)
	{
		bIsSelected = false;
		OnSelectionChanged.Broadcast(this, false);
	}

	Super::EndPlay(EndPlayReason);
}

void AModularBaseActor::ApplyItemData(const UBaseItemData& Data)
{
	// No mesh in the data means the subclass/Blueprint defines its own visuals, so leave them untouched.
	if (Data.Mesh.IsNull())
	{
		return;
	}

	MeshComponent->SetStaticMesh(Data.Mesh.LoadSynchronous());

	// Clear overrides from a previously applied item so slots fall back to the new mesh's defaults.
	MeshComponent->EmptyOverrideMaterials();
	for (int32 SlotIndex = 0; SlotIndex < Data.MaterialOverrides.Num(); ++SlotIndex)
	{
		if (!Data.MaterialOverrides[SlotIndex].IsNull())
		{
			MeshComponent->SetMaterial(SlotIndex, Data.MaterialOverrides[SlotIndex].LoadSynchronous());
		}
	}
}

void AModularBaseActor::UpdateHighlight()
{
	UMaterialInterface* Overlay = bIsSelected ? SelectedOverlayMaterial.Get() : (bIsHovered ? HoverOverlayMaterial.Get() : nullptr);
	MeshComponent->SetOverlayMaterial(Overlay);
}

void AModularBaseActor::OnHoverBegin_Implementation()
{
	if (!bIsHovered)
	{
		bIsHovered = true;
		UpdateHighlight();
	}
}

void AModularBaseActor::OnHoverEnd_Implementation()
{
	if (bIsHovered)
	{
		bIsHovered = false;
		UpdateHighlight();
	}
}

void AModularBaseActor::OnSelect_Implementation()
{
	if (!bIsSelected)
	{
		bIsSelected = true;
		UpdateHighlight();
		OnSelectionChanged.Broadcast(this, true);
	}
}

void AModularBaseActor::OnDeselect_Implementation()
{
	if (bIsSelected)
	{
		bIsSelected = false;
		UpdateHighlight();
		OnSelectionChanged.Broadcast(this, false);
	}
}

FStageItemInteractionDetails AModularBaseActor::GetInteractionDetails_Implementation() const
{
	FStageItemInteractionDetails Details;
	Details.ItemData = ItemData;

	if (ItemData)
	{
		Details.DisplayName = ItemData->DisplayName;
		Details.ItemType = ItemData->ItemType;
	}
	else
	{
		Details.DisplayName = FText::FromString(GetActorNameOrLabel());
	}

	// Color editing is opt-in: light subclasses override this and fill bSupportsColorEditing/Color.
	return Details;
}
