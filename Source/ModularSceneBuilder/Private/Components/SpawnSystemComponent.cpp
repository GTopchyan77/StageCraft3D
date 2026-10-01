// Copyright Epic Games, Inc. All Rights Reserved.

#include "Components/SpawnSystemComponent.h"

#include "Actors/ModularBaseActor.h"
#include "Data/BaseItemData.h"
#include "Engine/GameInstance.h"
#include "Engine/HitResult.h"
#include "Engine/World.h"
#include "Interaction/InteractableInterface.h"
#include "ModularSceneBuilder.h"
#include "Subsystems/StageItemSubsystem.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(SpawnSystemComponent)

USpawnSystemComponent::USpawnSystemComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
}

void USpawnSystemComponent::BeginPlay()
{
	Super::BeginPlay();

	const UGameInstance* GameInstance = GetWorld() ? GetWorld()->GetGameInstance() : nullptr;
	ItemSubsystem = GameInstance ? GameInstance->GetSubsystem<UStageItemSubsystem>() : nullptr;

	if (ItemSubsystem)
	{
		ItemSubsystem->OnSelectedItemChanged.AddDynamic(this, &ThisClass::HandleSelectedItemChanged);
		// Selection may have been made before this component existed (e.g. in a menu level).
		ActiveItem = ItemSubsystem->GetSelectedItem();
	}
	else
	{
		UE_LOG(LogStageCraft, Error, TEXT("%s: UStageItemSubsystem not found; spawning is disabled."), *GetNameSafe(this));
	}
}

void USpawnSystemComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (ItemSubsystem)
	{
		ItemSubsystem->OnSelectedItemChanged.RemoveDynamic(this, &ThisClass::HandleSelectedItemChanged);
		ItemSubsystem = nullptr;
	}

	EndPlacement();
	Super::EndPlay(EndPlayReason);
}

void USpawnSystemComponent::HandleSelectedItemChanged(UBaseItemData* NewItem, UBaseItemData* PreviousItem)
{
	// A stroke belongs to one item; switching mid-drag must not keep painting the old one.
	EndPlacement();
	ActiveItem = NewItem;
}

void USpawnSystemComponent::BeginPlacement(const FHitResult& Hit)
{
	EndPlacement();

	if (!ActiveItem || !Hit.bBlockingHit)
	{
		return;
	}

	const FTransform Transform = ComputePlacementTransform(*ActiveItem, Hit);
	if (!SpawnItemAt(*ActiveItem, Transform))
	{
		return;
	}

	if (ActiveItem->PlacementRules.PlacementMode == EStageItemPlacementMode::Continuous)
	{
		bStrokeActive = true;
		StrokeCells.Add(ToStrokeCell(*ActiveItem, Transform.GetLocation()));
	}
}

void USpawnSystemComponent::UpdatePlacement(const FHitResult& Hit)
{
	if (!bStrokeActive || !ActiveItem || !Hit.bBlockingHit)
	{
		return;
	}

	const FTransform Transform = ComputePlacementTransform(*ActiveItem, Hit);
	const FIntPoint Cell = ToStrokeCell(*ActiveItem, Transform.GetLocation());

	// Also stops stacking: while the cursor rests on the item just placed, the hit lands on its top
	// in the same XY cell and is rejected here.
	if (StrokeCells.Contains(Cell))
	{
		return;
	}

	if (SpawnItemAt(*ActiveItem, Transform))
	{
		StrokeCells.Add(Cell);
	}
}

void USpawnSystemComponent::EndPlacement()
{
	bStrokeActive = false;
	StrokeCells.Reset();
}

bool USpawnSystemComponent::TryDeleteActor(AActor* Target)
{
	if (!IsValid(Target) || Target->IsActorBeingDestroyed() || !Target->Implements<UInteractableInterface>())
	{
		return false;
	}

	UE_LOG(LogStageCraft, Log, TEXT("Deleting stage item %s."), *Target->GetName());
	OnItemDeleted.Broadcast(Target);
	// Selection listeners are released by AModularBaseActor::EndPlay.
	return Target->Destroy();
}

AModularBaseActor* USpawnSystemComponent::SpawnItemAt(UBaseItemData& Item, const FTransform& Transform)
{
	UWorld* World = GetWorld();
	// Normally already resident: UStageItemSubsystem streams the Game bundle before selection completes.
	UClass* ActorClass = Item.ActorClass.LoadSynchronous();

	if (!World || !ActorClass)
	{
		UE_LOG(LogStageCraft, Warning, TEXT("Cannot spawn %s: missing world or ActorClass."), *Item.GetName());
		return nullptr;
	}

	if (PlacementValidator.IsBound())
	{
		const FStageEconomyResultInfo Verdict = PlacementValidator.Execute(Item);
		if (!Verdict.IsSuccess())
		{
			// Stop stamping: the next cell of the stroke would be refused for the same reason.
			EndPlacement();
			return nullptr;
		}
	}

	// Deferred so construction scripts and BeginPlay already see the item data.
	AModularBaseActor* Actor = World->SpawnActorDeferred<AModularBaseActor>(ActorClass, Transform, nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (!Actor)
	{
		return nullptr;
	}

	Actor->InitializeFromItemData(&Item);
	Actor->FinishSpawning(Transform);

	OnItemSpawned.Broadcast(Actor);
	return Actor;
}

FTransform USpawnSystemComponent::ComputePlacementTransform(const UBaseItemData& Item, const FHitResult& Hit) const
{
	const FStageItemPlacementRules& Rules = Item.PlacementRules;

	FVector Location = Hit.ImpactPoint;
	Location.X = Rules.GridSize.X > 0.0 ? FMath::GridSnap(Location.X, Rules.GridSize.X) : Location.X;
	Location.Y = Rules.GridSize.Y > 0.0 ? FMath::GridSnap(Location.Y, Rules.GridSize.Y) : Location.Y;
	Location.Z = Rules.GridSize.Z > 0.0 ? FMath::GridSnap(Location.Z, Rules.GridSize.Z) : Location.Z;

	const FQuat Rotation = Rules.bAlignToSurfaceNormal
		? FRotationMatrix::MakeFromZ(Hit.ImpactNormal).ToQuat()
		: FQuat::Identity;

	Location += Rotation.RotateVector(Rules.PlacementOffset);

	return FTransform(Rotation, Location);
}

FIntPoint USpawnSystemComponent::ToStrokeCell(const UBaseItemData& Item, const FVector& Location) const
{
	const FVector& Grid = Item.PlacementRules.GridSize;
	const double CellX = Grid.X > 0.0 ? Grid.X : FallbackStrokeCellSize;
	const double CellY = Grid.Y > 0.0 ? Grid.Y : FallbackStrokeCellSize;

	return FIntPoint(FMath::RoundToInt32(Location.X / CellX), FMath::RoundToInt32(Location.Y / CellY));
}
