// Copyright Epic Games, Inc. All Rights Reserved.

#include "History/StageEditHistoryComponent.h"

#include "Actors/ModularBaseActor.h"
#include "Components/SelectionComponent.h"
#include "Components/SpawnSystemComponent.h"
#include "Data/BaseItemData.h"
#include "Engine/World.h"
#include "History/StageEditHistorySubsystem.h"
#include "History/StageItemCommands.h"
#include "History/StageItemSnapshot.h"
#include "Interaction/StageTransformRules.h"
#include "ModularSceneBuilder.h"
#include "Placement/StagePlacementToolComponent.h"
#include "Subsystems/StageSessionSubsystem.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageEditHistoryComponent)

namespace StageEditHistory
{
	/** Removal shared by Delete and by undo/redo: the selection lets go first, so the gizmo and inspector never see a dying actor. */
	bool RemoveStageItem(AActor& Target, USpawnSystemComponent& SpawnSystem, USelectionComponent& Selection)
	{
		if (Selection.GetSelectedActor() == &Target)
		{
			Selection.ClearSelection();
		}
		return SpawnSystem.TryDeleteActor(&Target);
	}

	/**
	 * IStageItemEditor over the live world, alive only for one undo/redo step. Holds references only,
	 * all of which the owning component guarantees for that duration.
	 */
	class FLiveItemEditor final : public IStageItemEditor
	{
	public:
		FLiveItemEditor(UStageSessionSubsystem& InSession, USpawnSystemComponent& InSpawnSystem, USelectionComponent& InSelection)
			: Session(InSession), SpawnSystem(InSpawnSystem), Selection(InSelection)
		{
		}

		virtual bool CaptureItem(const FGuid& InstanceId, FStageItemSnapshot& OutSnapshot) const override
		{
			const AModularBaseActor* Item = Session.FindItemById(InstanceId);
			if (!Item)
			{
				return false;
			}
			OutSnapshot = Item->CaptureSnapshot();
			return true;
		}

		virtual EStageCommandResult RestoreItem(const FStageItemSnapshot& Snapshot) override
		{
			// Two live items with one id would make every later step ambiguous.
			if (Session.FindItemById(Snapshot.InstanceId))
			{
				return EStageCommandResult::Invalid;
			}
			if (Snapshot.Item.LoadSynchronous() == nullptr)
			{
				return EStageCommandResult::Invalid;
			}
			// A null result with a valid item means the placement rules said no; the validator already reported it.
			return SpawnSystem.RestoreItem(Snapshot) ? EStageCommandResult::Succeeded : EStageCommandResult::Refused;
		}

		virtual EStageCommandResult RemoveItem(const FGuid& InstanceId) override
		{
			AModularBaseActor* Item = Session.FindItemById(InstanceId);
			if (!Item)
			{
				return EStageCommandResult::Invalid;
			}
			return RemoveStageItem(*Item, SpawnSystem, Selection) ? EStageCommandResult::Succeeded : EStageCommandResult::Invalid;
		}

		virtual EStageCommandResult SetItemTransform(const FGuid& InstanceId, const FTransform& Transform) override
		{
			AModularBaseActor* Item = Session.FindItemById(InstanceId);
			if (!Item)
			{
				return EStageCommandResult::Invalid;
			}

			// Same limits as the gizmo and inspector, in case the step was recorded before a limit changed.
			FTransform Clamped = Transform;
			Clamped.SetScale3D(StageTransformRules::ClampScale(Transform.GetScale3D()));
			// The session marks itself dirty and views refresh through the root's TransformUpdated, as for a gizmo move.
			Item->SetActorTransform(Clamped);
			return EStageCommandResult::Succeeded;
		}

	private:
		UStageSessionSubsystem& Session;
		USpawnSystemComponent& SpawnSystem;
		USelectionComponent& Selection;
	};
}

UStageEditHistoryComponent::UStageEditHistoryComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
}

void UStageEditHistoryComponent::BeginPlay()
{
	Super::BeginPlay();

	SpawnSystem = GetOwner()->FindComponentByClass<USpawnSystemComponent>();
	Selection = GetOwner()->FindComponentByClass<USelectionComponent>();
	ensureMsgf(SpawnSystem && Selection, TEXT("%s: UStageEditHistoryComponent needs USpawnSystemComponent and USelectionComponent on the same actor; undo is disabled."),
		*GetNameSafe(GetOwner()));

	PlacementTool = GetOwner()->FindComponentByClass<UStagePlacementToolComponent>();
	if (PlacementTool)
	{
		PlacementTool->OnItemPlaced.AddUniqueDynamic(this, &ThisClass::HandleItemPlaced);
	}
}

void UStageEditHistoryComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (PlacementTool)
	{
		PlacementTool->OnItemPlaced.RemoveDynamic(this, &ThisClass::HandleItemPlaced);
		PlacementTool = nullptr;
	}
	Super::EndPlay(EndPlayReason);
}

UStageEditHistorySubsystem* UStageEditHistoryComponent::GetHistory() const
{
	return UWorld::GetSubsystem<UStageEditHistorySubsystem>(GetWorld());
}

void UStageEditHistoryComponent::Record(TSharedRef<IStageEditCommand> Command) const
{
	if (UStageEditHistorySubsystem* History = GetHistory())
	{
		History->Record(MoveTemp(Command));
	}
}

void UStageEditHistoryComponent::HandleItemPlaced(AModularBaseActor* PlacedActor)
{
	if (IsValid(PlacedActor))
	{
		Record(MakeShared<FStagePlaceItemCommand>(PlacedActor->CaptureSnapshot()));
	}
}

bool UStageEditHistoryComponent::DeleteItem(AActor* Target)
{
	AModularBaseActor* Item = Cast<AModularBaseActor>(Target);
	if (!IsValid(Item) || Item->IsActorBeingDestroyed() || !SpawnSystem || !Selection)
	{
		return false;
	}

	// Captured while the actor is intact; undo brings back exactly this.
	FStageItemSnapshot Snapshot = Item->CaptureSnapshot();
	if (!StageEditHistory::RemoveStageItem(*Item, *SpawnSystem, *Selection))
	{
		return false;
	}

	Record(MakeShared<FStageDeleteItemCommand>(MoveTemp(Snapshot)));
	return true;
}

void UStageEditHistoryComponent::RecordTransformChange(AActor* Target, const FTransform& Before)
{
	const AModularBaseActor* Item = Cast<AModularBaseActor>(Target);
	if (!IsValid(Item))
	{
		return;
	}

	const FTransform After = Item->GetActorTransform();
	if (FStageTransformItemCommand::IsMeaningfulChange(Before, After))
	{
		Record(MakeShared<FStageTransformItemCommand>(Item->GetInstanceId(), Item->GetInstanceLabel(), Before, After));
	}
}

EStageCommandResult UStageEditHistoryComponent::Undo()
{
	return Step(/*bUndo*/ true);
}

EStageCommandResult UStageEditHistoryComponent::Redo()
{
	return Step(/*bUndo*/ false);
}

EStageCommandResult UStageEditHistoryComponent::Step(bool bUndo)
{
	UStageEditHistorySubsystem* History = GetHistory();
	UStageSessionSubsystem* Session = UWorld::GetSubsystem<UStageSessionSubsystem>(GetWorld());
	if (!History || !Session || !SpawnSystem || !Selection)
	{
		return EStageCommandResult::NothingToDo;
	}

	StageEditHistory::FLiveItemEditor Editor(*Session, *SpawnSystem, *Selection);
	return bUndo ? History->Undo(Editor) : History->Redo(Editor);
}
