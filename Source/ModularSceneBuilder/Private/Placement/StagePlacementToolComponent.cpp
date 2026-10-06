// Copyright Epic Games, Inc. All Rights Reserved.

#include "Placement/StagePlacementToolComponent.h"

#include "Actors/ModularBaseActor.h"
#include "Data/BaseItemData.h"
#include "Engine/GameInstance.h"
#include "Engine/HitResult.h"
#include "Engine/World.h"
#include "ModularSceneBuilder.h"
#include "Placement/StagePlacementMath.h"
#include "Placement/StagePlacementPreview.h"
#include "Subsystems/StageItemSubsystem.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StagePlacementToolComponent)

UStagePlacementToolComponent::UStagePlacementToolComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	PreviewClass = AStagePlacementPreview::StaticClass();
}

void UStagePlacementToolComponent::BeginPlay()
{
	Super::BeginPlay();

	SpawnSystem = GetOwner()->FindComponentByClass<USpawnSystemComponent>();
	ensureMsgf(SpawnSystem, TEXT("%s: UStagePlacementToolComponent needs a USpawnSystemComponent on the same actor; placement is disabled."), *GetNameSafe(GetOwner()));

	const UGameInstance* GameInstance = GetWorld() ? GetWorld()->GetGameInstance() : nullptr;
	ItemSubsystem = GameInstance ? GameInstance->GetSubsystem<UStageItemSubsystem>() : nullptr;
	if (!ItemSubsystem)
	{
		UE_LOG(LogStageCraft, Error, TEXT("%s: UStageItemSubsystem not found; no item can be armed."), *GetNameSafe(GetOwner()));
		return;
	}

	ItemSubsystem->OnSelectedItemChanged.AddUniqueDynamic(this, &ThisClass::HandleSelectedItemChanged);
	// An item may have been armed before this component existed (e.g. before level travel).
	SetArmedItem(ItemSubsystem->GetSelectedItem());
}

void UStagePlacementToolComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (ItemSubsystem)
	{
		ItemSubsystem->OnSelectedItemChanged.RemoveDynamic(this, &ThisClass::HandleSelectedItemChanged);
		ItemSubsystem = nullptr;
	}

	if (Preview)
	{
		Preview->Destroy();
		Preview = nullptr;
	}

	PreviewEvaluator.Unbind();
	Super::EndPlay(EndPlayReason);
}

void UStagePlacementToolComponent::EnterPlaceMode()
{
	if (EditMode == EStageEditMode::Place)
	{
		return;
	}

	// Rules can change between visits (purchases, session limits), so the verdict is refreshed on entry.
	EvaluateArmedItem();
	SetEditMode(EStageEditMode::Place);
}

void UStagePlacementToolComponent::EnterSelectMode()
{
	HidePreview();
	SetEditMode(EStageEditMode::Select);
}

void UStagePlacementToolComponent::TogglePlaceMode()
{
	if (IsPlaceMode())
	{
		EnterSelectMode();
	}
	else
	{
		EnterPlaceMode();
	}
}

void UStagePlacementToolComponent::UpdateTarget(const FHitResult& Hit)
{
	if (!IsPlaceMode() || !ArmedItem || !Hit.bBlockingHit)
	{
		HidePreview();
		return;
	}

	const FStageItemPlacementRules& Rules = ArmedItem->PlacementRules;
	const FTransform ItemTransform = StagePlacementMath::ComputePlacementTransform(Rules, Hit.ImpactPoint, Hit.ImpactNormal);
	// The snapped point on the surface, before the item's pivot offset: what the marker highlights.
	const FVector LandingPoint = ItemTransform.GetLocation() - ItemTransform.GetRotation().RotateVector(Rules.PlacementOffset);

	if (StagePlacementMath::UsesGridSnapping(Rules))
	{
		NotifyIfSnapped(LandingPoint);
	}
	LastLandingPoint = LandingPoint;

	if (AStagePlacementPreview* PreviewActor = GetOrCreatePreview())
	{
		PreviewState = ArmedItemVerdict;
		PreviewActor->ShowAt(ItemTransform, LandingPoint, Hit.ImpactNormal, PreviewState);
	}
}

void UStagePlacementToolComponent::ClearTarget()
{
	HidePreview();
}

AModularBaseActor* UStagePlacementToolComponent::TryPlace(const FHitResult& Hit)
{
	if (!IsPlaceMode() || !SpawnSystem)
	{
		return nullptr;
	}

	if (!ArmedItem)
	{
		OnPlacementFailed.Broadcast(EStagePlacementFailure::NoItemArmed);
		return nullptr;
	}

	if (!Hit.bBlockingHit)
	{
		OnPlacementFailed.Broadcast(EStagePlacementFailure::NoSurface);
		return nullptr;
	}

	// The exact transform the preview shows for this hit.
	const FTransform ItemTransform = StagePlacementMath::ComputePlacementTransform(ArmedItem->PlacementRules, Hit.ImpactPoint, Hit.ImpactNormal);
	AModularBaseActor* Placed = SpawnSystem->SpawnItem(ArmedItem, ItemTransform);
	if (!Placed)
	{
		// Refused or failed: stay in Place mode so the user can try elsewhere or press Esc.
		return nullptr;
	}

	// Stamping: Place mode and the ghost stay until the user leaves explicitly (Esc, P, the Library toggle).
	// The placement may have used up a session limit, so the verdict the ghost shows is refreshed now.
	EvaluateArmedItem();
	// The next preview lands on the new item; that jump is not a snap the user made, so it must not play the snap cue.
	LastLandingPoint.Reset();
	OnItemPlaced.Broadcast(Placed);
	return Placed;
}

void UStagePlacementToolComponent::HandleSelectedItemChanged(UBaseItemData* NewItem, UBaseItemData* PreviousItem)
{
	SetArmedItem(NewItem);
}

void UStagePlacementToolComponent::SetEditMode(EStageEditMode NewMode)
{
	if (NewMode == EditMode)
	{
		return;
	}

	const EStageEditMode PreviousMode = EditMode;
	EditMode = NewMode;
	UE_LOG(LogStageCraft, Log, TEXT("%s: edit mode %s -> %s."), *GetNameSafe(GetOwner()),
		*UEnum::GetValueAsString(PreviousMode), *UEnum::GetValueAsString(NewMode));
	OnEditModeChanged.Broadcast(NewMode, PreviousMode);
}

void UStagePlacementToolComponent::SetArmedItem(UBaseItemData* NewItem)
{
	ArmedItem = NewItem;
	EvaluateArmedItem();

	// A preview created later picks the item up in GetOrCreatePreview.
	if (Preview)
	{
		Preview->SetItem(ArmedItem);
	}

	// The ghost is repositioned by the controller's next UpdateTarget, which it schedules on this event.
	HidePreview();
	OnArmedItemChanged.Broadcast(ArmedItem);
}

void UStagePlacementToolComponent::EvaluateArmedItem()
{
	const bool bAllowed = !ArmedItem || !PreviewEvaluator.IsBound() || PreviewEvaluator.Execute(*ArmedItem).IsSuccess();
	ArmedItemVerdict = bAllowed ? EStagePlacementPreviewState::Valid : EStagePlacementPreviewState::Refused;
}

void UStagePlacementToolComponent::HidePreview()
{
	PreviewState = EStagePlacementPreviewState::Hidden;
	LastLandingPoint.Reset();
	if (Preview)
	{
		Preview->HidePreview();
	}
}

void UStagePlacementToolComponent::NotifyIfSnapped(const FVector& LandingPoint)
{
	// The first point after showing is not a snap; only moving between grid points is.
	if (LastLandingPoint.IsSet() && !LastLandingPoint->Equals(LandingPoint, UE_KINDA_SMALL_NUMBER))
	{
		OnPlacementSnapped.Broadcast(LandingPoint);
	}
}

AStagePlacementPreview* UStagePlacementToolComponent::GetOrCreatePreview()
{
	if (Preview || !PreviewClass)
	{
		return Preview;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		return nullptr;
	}

	FActorSpawnParameters SpawnParams;
	SpawnParams.Owner = GetOwner();
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	SpawnParams.ObjectFlags |= RF_Transient;
	Preview = World->SpawnActor<AStagePlacementPreview>(PreviewClass, FTransform::Identity, SpawnParams);
	if (Preview)
	{
		Preview->SetItem(ArmedItem);
	}
	return Preview;
}
