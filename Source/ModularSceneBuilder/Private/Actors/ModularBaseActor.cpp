// Copyright Epic Games, Inc. All Rights Reserved.

#include "Actors/ModularBaseActor.h"

#include "Components/StaticMeshComponent.h"
#include "Data/BaseItemData.h"
#include "Engine/CollisionProfile.h"
#include "Engine/StaticMesh.h"
#include "Interaction/StageCraftCollision.h"
#include "Materials/MaterialInterface.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(ModularBaseActor)

#define LOCTEXT_NAMESPACE "StageCraftModularBaseActor"

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

void AModularBaseActor::BeginPlay()
{
	Super::BeginPlay();

	// Only the root is watched: child moves (e.g. a moving head's pan/tilt pivots) are attributes, not transform.
	RootTransformUpdatedHandle = MeshComponent->TransformUpdated.AddUObject(this, &ThisClass::HandleRootTransformUpdated);
}

void AModularBaseActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	MeshComponent->TransformUpdated.Remove(RootTransformUpdatedHandle);
	RootTransformUpdatedHandle.Reset();

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

FText AModularBaseActor::GetInstanceLabel() const
{
	if (!InstanceLabel.IsEmpty())
	{
		return InstanceLabel;
	}
	return ItemData ? ItemData->DisplayName : FText::FromString(GetActorNameOrLabel());
}

TArray<FStageParameterSection> AModularBaseActor::GetParameterSections_Implementation() const
{
	TArray<FStageParameterSection> Sections;
	GatherParameterSections(Sections);
	return Sections;
}

bool AModularBaseActor::GetParameterValue_Implementation(FGameplayTag ParameterId, FStageParameterValue& OutValue) const
{
	return ReadParameter(ParameterId, OutValue);
}

bool AModularBaseActor::SetParameterValue_Implementation(FGameplayTag ParameterId, const FStageParameterValue& Value)
{
	return WriteParameter(ParameterId, Value);
}

void AModularBaseActor::GatherParameterSections(TArray<FStageParameterSection>& OutSections) const
{
	FStageParameterSection& Info = OutSections.Emplace_GetRef(StageCraftTags::FeatureGroup_Info, LOCTEXT("InfoSection", "Info"));
	Info.Add(StageCraftTags::Param_Info_Label, LOCTEXT("Label", "Label"), FStageParameterValue::MakeText(GetInstanceLabel()));

	if (ItemData)
	{
		Info.Add(StageCraftTags::Param_Info_Model, LOCTEXT("Model", "Model"), FStageParameterValue::MakeText(ItemData->Specs.Manufacturer.IsEmpty()
			? ItemData->DisplayName
			: FText::Format(LOCTEXT("ModelFormat", "{0} {1}"), ItemData->Specs.Manufacturer, ItemData->DisplayName))).ReadOnly();

		if (ItemData->Specs.WeightKg > 0.f)
		{
			Info.Add(StageCraftTags::Param_Info_Weight, LOCTEXT("Weight", "Weight"), FStageParameterValue::MakeFloat(ItemData->Specs.WeightKg))
				.Display(1.0, LOCTEXT("Kg", "kg")).ReadOnly();
		}
		if (ItemData->Specs.PowerDrawWatts > 0.f)
		{
			Info.Add(StageCraftTags::Param_Info_Power, LOCTEXT("Power", "Power"), FStageParameterValue::MakeFloat(ItemData->Specs.PowerDrawWatts))
				.Display(1.0, LOCTEXT("Watt", "W")).ReadOnly();
		}
	}

	FStageParameterSection& Transform = OutSections.Emplace_GetRef(StageCraftTags::FeatureGroup_Transform, LOCTEXT("TransformSection", "Transform"));
	Transform.Add(StageCraftTags::Param_Transform_Location, LOCTEXT("Location", "Location"), FStageParameterValue::MakeVector(GetActorLocation()))
		.Display(1.0, LOCTEXT("Cm", "cm"), 1.0);
	Transform.Add(StageCraftTags::Param_Transform_Rotation, LOCTEXT("Rotation", "Rotation"), FStageParameterValue::MakeRotator(GetActorRotation()))
		.Display(1.0, LOCTEXT("Deg", "deg"), 1.0);
}

bool AModularBaseActor::ReadParameter(const FGameplayTag& ParameterId, FStageParameterValue& OutValue) const
{
	if (ParameterId == StageCraftTags::Param_Info_Label)
	{
		OutValue = FStageParameterValue::MakeText(GetInstanceLabel());
		return true;
	}
	if (ParameterId == StageCraftTags::Param_Transform_Location)
	{
		OutValue = FStageParameterValue::MakeVector(GetActorLocation());
		return true;
	}
	if (ParameterId == StageCraftTags::Param_Transform_Rotation)
	{
		OutValue = FStageParameterValue::MakeRotator(GetActorRotation());
		return true;
	}
	return false;
}

bool AModularBaseActor::WriteParameter(const FGameplayTag& ParameterId, const FStageParameterValue& Value)
{
	if (ParameterId == StageCraftTags::Param_Info_Label && Value.Type == EStageParameterType::Text)
	{
		InstanceLabel = Value.Text;
		NotifyParameterChanged(ParameterId);
		return true;
	}
	// Transform writes notify through HandleRootTransformUpdated, so gizmo and inspector share one path.
	if (ParameterId == StageCraftTags::Param_Transform_Location && Value.Type == EStageParameterType::Vector)
	{
		SetActorLocation(Value.Vector);
		return true;
	}
	if (ParameterId == StageCraftTags::Param_Transform_Rotation && Value.Type == EStageParameterType::Rotator)
	{
		SetActorRotation(Value.Rotator);
		return true;
	}
	return false;
}

void AModularBaseActor::NotifyParameterChanged(const FGameplayTag& ParameterId)
{
	OnParameterChanged.Broadcast(this, ParameterId);
}

void AModularBaseActor::HandleRootTransformUpdated(USceneComponent* UpdatedComponent, EUpdateTransformFlags UpdateTransformFlags, ETeleportType Teleport)
{
	NotifyParameterChanged(StageCraftTags::Param_Transform_Location);
	NotifyParameterChanged(StageCraftTags::Param_Transform_Rotation);
}

#undef LOCTEXT_NAMESPACE
