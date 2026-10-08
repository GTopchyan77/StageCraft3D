// Copyright Epic Games, Inc. All Rights Reserved.

#include "Actors/ModularBaseActor.h"

#include "Components/StaticMeshComponent.h"
#include "Data/BaseItemData.h"
#include "Engine/CollisionProfile.h"
#include "Engine/GameInstance.h"
#include "Engine/StaticMesh.h"
#include "History/StageItemSnapshot.h"
#include "Interaction/StageCraftCollision.h"
#include "Interaction/StageTransformRules.h"
#include "Materials/MaterialInterface.h"
#include "ModularSceneBuilder.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#include "Serialization/ObjectAndNameAsStringProxyArchive.h"
#include "Subsystems/StageItemSubsystem.h"
#include "Subsystems/StageSessionSubsystem.h"

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

	// A live swap changes which sections and rows exist; views rebuild instead of refreshing rows.
	if (HasActorBegunPlay())
	{
		NotifyParameterChanged(FGameplayTag());
	}
}

void AModularBaseActor::GetSwappableItems(TArray<UBaseItemData*>& OutItems) const
{
	OutItems.Reset();
	const UGameInstance* GameInstance = GetGameInstance();
	const UStageItemSubsystem* Catalog = GameInstance ? GameInstance->GetSubsystem<UStageItemSubsystem>() : nullptr;
	if (!ItemData || !Catalog)
	{
		return;
	}

	// Same data class and item type: ApplyItemData of this actor class understands every candidate,
	// so a moving head can become another moving head but never a speaker.
	for (UBaseItemData* Candidate : Catalog->GetCatalog())
	{
		if (Candidate && Candidate->GetClass() == ItemData->GetClass() && Candidate->ItemType == ItemData->ItemType)
		{
			OutItems.Add(Candidate);
		}
	}

	// Level-placed instances may use an item outside the scanned catalog; it must still be listed.
	if (!OutItems.Contains(ItemData))
	{
		OutItems.Insert(ItemData, 0);
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

	// Restored items arrive with their old id (AssignInstanceId); everything else gets a fresh one.
	if (!InstanceId.IsValid())
	{
		InstanceId = FGuid::NewGuid();
	}

	// Only the root is watched: child moves (e.g. a moving head's pan/tilt pivots) are attributes, not transform.
	RootTransformUpdatedHandle = MeshComponent->TransformUpdated.AddUObject(this, &ThisClass::HandleRootTransformUpdated);

	if (UStageSessionSubsystem* Session = UWorld::GetSubsystem<UStageSessionSubsystem>(GetWorld()))
	{
		Session->RegisterItem(this);
	}
}

void AModularBaseActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (UStageSessionSubsystem* Session = UWorld::GetSubsystem<UStageSessionSubsystem>(GetWorld()))
	{
		Session->UnregisterItem(this);
	}

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
	UMaterialInterface* Overlay = bHighlightSuppressed ? nullptr
		: (bIsSelected ? SelectedOverlayMaterial.Get() : (bIsHovered ? HoverOverlayMaterial.Get() : nullptr));
	MeshComponent->SetOverlayMaterial(Overlay);
}

void AModularBaseActor::SetHighlightSuppressed(bool bSuppressed)
{
	if (bHighlightSuppressed != bSuppressed)
	{
		bHighlightSuppressed = bSuppressed;
		UpdateHighlight();
	}
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

void AModularBaseActor::AssignInstanceId(const FGuid& RestoredId)
{
	// After BeginPlay the session has registered the current id; changing it would orphan history entries.
	if (!ensureMsgf(!HasActorBegunPlay() && RestoredId.IsValid(), TEXT("%s: AssignInstanceId is only valid with a valid id, before BeginPlay."), *GetName()))
	{
		return;
	}
	InstanceId = RestoredId;
}

FStageItemSnapshot AModularBaseActor::CaptureSnapshot() const
{
	FStageItemSnapshot Snapshot;
	Snapshot.InstanceId = InstanceId;
	Snapshot.Item = ItemData;
	Snapshot.Transform = GetActorTransform();
	Snapshot.DisplayName = GetInstanceLabel();

	// Only SaveGame properties: the per-instance state a user can edit, not components or engine state.
	FMemoryWriter Writer(Snapshot.SavedProperties, /*bIsPersistent*/ false);
	FObjectAndNameAsStringProxyArchive Archive(Writer, /*bInLoadIfFindFails*/ false);
	Archive.ArIsSaveGame = true;
	Archive.ArNoDelta = true;
	SerializeScriptProperties(Archive);
	return Snapshot;
}

bool AModularBaseActor::RestoreSnapshotState(const FStageItemSnapshot& Snapshot)
{
	if (Snapshot.SavedProperties.IsEmpty())
	{
		return true;
	}

	FMemoryReader Reader(Snapshot.SavedProperties, /*bIsPersistent*/ false);
	FObjectAndNameAsStringProxyArchive Archive(Reader, /*bInLoadIfFindFails*/ true);
	Archive.ArIsSaveGame = true;
	Archive.ArNoDelta = true;
	SerializeScriptProperties(Archive);

	if (Archive.IsError())
	{
		UE_LOG(LogStageCraft, Warning, TEXT("%s: could not read the saved state of %s; it is restored with default settings."), *GetName(), *Snapshot.DisplayName.ToString());
		return false;
	}
	return true;
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

		TArray<UBaseItemData*> Swappable;
		GetSwappableItems(Swappable);
		if (Swappable.Num() > 1)
		{
			TArray<FText> Names;
			Names.Reserve(Swappable.Num());
			for (const UBaseItemData* Item : Swappable)
			{
				Names.Add(Item->DisplayName);
			}
			Info.Add(StageCraftTags::Param_Info_Type, LOCTEXT("Type", "Type"), FStageParameterValue::MakeEnum(Swappable.IndexOfByKey(ItemData)))
				.WithOptions(MoveTemp(Names));
		}
	}

	FStageParameterSection& Transform = OutSections.Emplace_GetRef(StageCraftTags::FeatureGroup_Transform, LOCTEXT("TransformSection", "Transform"));
	Transform.Add(StageCraftTags::Param_Transform_Location, LOCTEXT("Location", "Location"), FStageParameterValue::MakeVector(GetActorLocation()))
		.Display(1.0, LOCTEXT("Cm", "cm"), 1.0);
	Transform.Add(StageCraftTags::Param_Transform_Rotation, LOCTEXT("Rotation", "Rotation"), FStageParameterValue::MakeRotator(GetActorRotation()))
		.Display(1.0, LOCTEXT("Deg", "°"), 1.0);
	Transform.Add(StageCraftTags::Param_Transform_Scale, LOCTEXT("Scale", "Scale"), FStageParameterValue::MakeVector(GetActorScale3D()))
		.Range(StageTransformRules::MinScale, StageTransformRules::MaxScale)
		.Display(1.0, LOCTEXT("Times", "×"), 0.01);
}

bool AModularBaseActor::ReadParameter(const FGameplayTag& ParameterId, FStageParameterValue& OutValue) const
{
	if (ParameterId == StageCraftTags::Param_Info_Label)
	{
		OutValue = FStageParameterValue::MakeText(GetInstanceLabel());
		return true;
	}
	if (ParameterId == StageCraftTags::Param_Info_Type)
	{
		TArray<UBaseItemData*> Swappable;
		GetSwappableItems(Swappable);
		OutValue = FStageParameterValue::MakeEnum(Swappable.IndexOfByKey(ItemData));
		return Swappable.Num() > 1;
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
	if (ParameterId == StageCraftTags::Param_Transform_Scale)
	{
		OutValue = FStageParameterValue::MakeVector(GetActorScale3D());
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
	if (ParameterId == StageCraftTags::Param_Info_Type && Value.Type == EStageParameterType::Enum)
	{
		TArray<UBaseItemData*> Swappable;
		GetSwappableItems(Swappable);
		if (!Swappable.IsValidIndex(Value.Integer))
		{
			return false;
		}
		// Keeps transform, label, fixture ID and patch; ApplyItemData adapts the rest (meshes, attribute set).
		if (Swappable[Value.Integer] != ItemData)
		{
			InitializeFromItemData(Swappable[Value.Integer]);
		}
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
	if (ParameterId == StageCraftTags::Param_Transform_Scale && Value.Type == EStageParameterType::Vector)
	{
		// Same limits as the gizmo; an out-of-range entry is clamped and the field reads the clamped value back.
		SetActorScale3D(StageTransformRules::ClampScale(Value.Vector));
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
	NotifyParameterChanged(StageCraftTags::Param_Transform_Scale);
}

#undef LOCTEXT_NAMESPACE
