// Copyright Epic Games, Inc. All Rights Reserved.

#include "History/StageEditHistoryComponent.h"

#include "Actors/ModularBaseActor.h"
#include "Components/SelectionComponent.h"
#include "Components/SpawnSystemComponent.h"
#include "Data/BaseItemData.h"
#include "Engine/World.h"
#include "History/StageBatchCommand.h"
#include "History/StageCueCommands.h"
#include "History/StageEditHistorySubsystem.h"
#include "History/StageItemCommands.h"
#include "History/StageItemSnapshot.h"
#include "Interaction/StageParameterInterface.h"
#include "Interaction/StageTransformRules.h"
#include "ModularSceneBuilder.h"
#include "Placement/StagePlacementToolComponent.h"
#include "Placement/StageSpanToolComponent.h"
#include "Subsystems/ShowControlSubsystem.h"
#include "Subsystems/StageSessionSubsystem.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageEditHistoryComponent)

namespace StageEditHistory
{
	/** Removal shared by Delete and by undo/redo: the selection lets go first, so the gizmo and inspector never see a dying actor. */
	bool RemoveStageItem(AActor& Target, USpawnSystemComponent& SpawnSystem, USelectionComponent& Selection)
	{
		Selection.DeselectActor(&Target);
		return SpawnSystem.TryDeleteActor(&Target);
	}

	/**
	 * IStageItemEditor over the live world, alive only for one undo/redo step. Holds references only,
	 * all of which the owning component guarantees for that duration.
	 */
	class FLiveItemEditor final : public IStageItemEditor
	{
	public:
		FLiveItemEditor(UStageSessionSubsystem& InSession, USpawnSystemComponent& InSpawnSystem, USelectionComponent& InSelection,
			const FStageParameterApplier& InParameterApplier, const FStageTransformValidator& InTransformValidator)
			: Session(InSession), SpawnSystem(InSpawnSystem), Selection(InSelection), ParameterApplier(InParameterApplier), TransformValidator(InTransformValidator)
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
			if (!StageTransformRules::IsSaneTransform(Clamped))
			{
				// A damaged step can never apply; the history drops it instead of offering it again.
				UE_LOG(LogStageCraft, Warning, TEXT("History: the transform for %s is not a valid stage position; the step is dropped."), *Item->GetInstanceLabel().ToString());
				return EStageCommandResult::Invalid;
			}
			// The rules decide again, for every item of a group step: a locked item refuses, and the batch rolls the others back.
			if (TransformValidator.IsBound() && !TransformValidator.Execute(*Item, Clamped))
			{
				return EStageCommandResult::Refused;
			}
			// The session marks itself dirty and views refresh through the root's TransformUpdated, as for a gizmo move.
			Item->SetActorTransform(Clamped);
			return EStageCommandResult::Succeeded;
		}

		virtual EStageCommandResult SetItemParameter(const FGuid& InstanceId, const FGameplayTag& ParameterId, const FStageParameterValue& Value) override
		{
			AModularBaseActor* Item = Session.FindItemById(InstanceId);
			if (!Item)
			{
				return EStageCommandResult::Invalid;
			}
			if (ParameterApplier.IsBound())
			{
				return ParameterApplier.Execute(*Item, ParameterId, Value);
			}
			return Session.ApplyParameterChange(Item, ParameterId, Value) ? EStageCommandResult::Succeeded : EStageCommandResult::Invalid;
		}

		virtual EStageCommandResult SetCueList(const TArray<FShowCue>& Cues) override
		{
			UShowControlSubsystem* Show = UWorld::GetSubsystem<UShowControlSubsystem>(Session.GetWorld());
			if (!Show)
			{
				return EStageCommandResult::Invalid;
			}
			// The session marks itself dirty through OnCueListChanged, as for a cue edit made in the panel.
			Show->ReplaceCues(Cues);
			return EStageCommandResult::Succeeded;
		}

	private:
		UStageSessionSubsystem& Session;
		USpawnSystemComponent& SpawnSystem;
		USelectionComponent& Selection;
		const FStageParameterApplier& ParameterApplier;
		const FStageTransformValidator& TransformValidator;
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

	SpanTool = GetOwner()->FindComponentByClass<UStageSpanToolComponent>();
	if (SpanTool)
	{
		SpanTool->OnItemPlaced.AddUniqueDynamic(this, &ThisClass::HandleItemPlaced);
	}
}

void UStageEditHistoryComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (PlacementTool)
	{
		PlacementTool->OnItemPlaced.RemoveDynamic(this, &ThisClass::HandleItemPlaced);
		PlacementTool = nullptr;
	}
	if (SpanTool)
	{
		SpanTool->OnItemPlaced.RemoveDynamic(this, &ThisClass::HandleItemPlaced);
		SpanTool = nullptr;
	}
	// An edit still being dragged when play ends is recorded, not lost.
	EndTransaction();
	ParameterApplier.Unbind();
	TransformValidator.Unbind();
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

void UStageEditHistoryComponent::RecordCueListChange(const FText& Description, const TArray<FShowCue>& Before, const TArray<FShowCue>& After)
{
	EndTransaction();
	Record(MakeShared<FStageCueListCommand>(Description, Before, After));
}

void UStageEditHistoryComponent::RecordPlacements(TConstArrayView<AModularBaseActor*> PlacedItems, const FText& Description)
{
	EndTransaction();
	TArray<TSharedRef<IStageEditCommand>> Commands;
	Commands.Reserve(PlacedItems.Num());
	for (const AModularBaseActor* Item : PlacedItems)
	{
		if (IsValid(Item))
		{
			Commands.Add(MakeShared<FStagePlaceItemCommand>(Item->CaptureSnapshot()));
		}
	}
	RecordAsOneStep(MoveTemp(Commands), Description);
}

void UStageEditHistoryComponent::RecordAsOneStep(TArray<TSharedRef<IStageEditCommand>> Commands, const FText& BatchDescription) const
{
	if (Commands.Num() == 1)
	{
		Record(Commands[0]);
	}
	else if (Commands.Num() > 1)
	{
		Record(MakeShared<FStageBatchCommand>(BatchDescription, MoveTemp(Commands)));
	}
}

bool UStageEditHistoryComponent::DeleteItem(AActor* Target)
{
	return DeleteItems({ Target }) == 1;
}

int32 UStageEditHistoryComponent::DeleteItems(const TArray<AActor*>& Targets)
{
	return DeleteAsOneStep(Targets, [](int32 Count)
	{
		return FText::Format(NSLOCTEXT("StageEditHistory", "DeleteItems", "Delete {0} items"), FText::AsNumber(Count));
	});
}

int32 UStageEditHistoryComponent::ClearStage()
{
	const UStageSessionSubsystem* Session = UWorld::GetSubsystem<UStageSessionSubsystem>(GetWorld());
	if (!Session)
	{
		return 0;
	}

	const TArray<AModularBaseActor*> PlacedItems = Session->GetPlacedItems();
	const int32 Removed = DeleteAsOneStep(TArray<AActor*>(PlacedItems), [](int32 Count)
	{
		return FText::Format(NSLOCTEXT("StageEditHistory", "ClearStage", "Clear Stage ({0} items)"), FText::AsNumber(Count));
	});
	UE_LOG(LogStageCraft, Log, TEXT("%s: cleared the stage (%d of %d items removed)."), *GetNameSafe(GetOwner()), Removed, PlacedItems.Num());
	return Removed;
}

int32 UStageEditHistoryComponent::DeleteAsOneStep(const TArray<AActor*>& Targets, TFunctionRef<FText(int32)> MakeBatchDescription)
{
	if (!SpawnSystem || !Selection)
	{
		return 0;
	}

	TArray<TSharedRef<IStageEditCommand>> Commands;
	Commands.Reserve(Targets.Num());
	for (AActor* Target : Targets)
	{
		AModularBaseActor* Item = Cast<AModularBaseActor>(Target);
		if (!IsValid(Item) || Item->IsActorBeingDestroyed())
		{
			continue;
		}

		// Captured while the actor is intact; undo brings back exactly this.
		FStageItemSnapshot Snapshot = Item->CaptureSnapshot();
		if (StageEditHistory::RemoveStageItem(*Item, *SpawnSystem, *Selection))
		{
			Commands.Add(MakeShared<FStageDeleteItemCommand>(MoveTemp(Snapshot)));
		}
	}

	const int32 Deleted = Commands.Num();
	RecordAsOneStep(MoveTemp(Commands), MakeBatchDescription(Deleted));
	return Deleted;
}

void UStageEditHistoryComponent::RecordTransformChange(AActor* Target, const FTransform& Before)
{
	const FStageTransformChange Change{ Target, Before };
	RecordTransformChanges(MakeArrayView(&Change, 1));
}

void UStageEditHistoryComponent::RecordTransformChanges(TConstArrayView<FStageTransformChange> Changes)
{
	if (Transaction.IsSet())
	{
		// Only the first "before" of each item counts: later ones are intermediate states of the same edit.
		for (const FStageTransformChange& Change : Changes)
		{
			const bool bKnown = Transaction->Transforms.ContainsByPredicate([&Change](const FStageTransformChange& Pending) { return Pending.Item == Change.Item; });
			if (!bKnown && Change.Item.IsValid())
			{
				Transaction->Transforms.Add(Change);
			}
		}
		return;
	}

	TArray<TSharedRef<IStageEditCommand>> Commands = MakeTransformCommands(Changes);
	const int32 Count = Commands.Num();
	RecordAsOneStep(MoveTemp(Commands), FText::Format(NSLOCTEXT("StageEditHistory", "TransformItems", "Transform {0} items"), FText::AsNumber(Count)));
}

TArray<TSharedRef<IStageEditCommand>> UStageEditHistoryComponent::MakeTransformCommands(TConstArrayView<FStageTransformChange> Changes)
{
	TArray<TSharedRef<IStageEditCommand>> Commands;
	for (const FStageTransformChange& Change : Changes)
	{
		const AModularBaseActor* Item = Cast<AModularBaseActor>(Change.Item.Get());
		if (!IsValid(Item))
		{
			continue;
		}

		const FTransform After = Item->GetActorTransform();
		if (FStageTransformItemCommand::IsMeaningfulChange(Change.Before, After))
		{
			Commands.Add(MakeShared<FStageTransformItemCommand>(Item->GetInstanceId(), Item->GetInstanceLabel(), Change.Before, After));
		}
	}
	return Commands;
}

void UStageEditHistoryComponent::RecordParameterChange(AActor* Target, const FGameplayTag& ParameterId, const FText& ParameterName, const FStageParameterValue& Before)
{
	AModularBaseActor* Item = Cast<AModularBaseActor>(Target);
	if (!IsValid(Item))
	{
		return;
	}

	if (Transaction.IsSet())
	{
		const bool bKnown = Transaction->Parameters.ContainsByPredicate([Item, &ParameterId](const FPendingParameterChange& Pending)
		{
			return Pending.Item == Item && Pending.ParameterId == ParameterId;
		});
		if (!bKnown)
		{
			Transaction->Parameters.Add({ Item, ParameterId, ParameterName, Before });
		}
		return;
	}

	FStageParameterValue After;
	if (!IStageParameterInterface::Execute_GetParameterValue(Item, ParameterId, After) || !FStageParameterItemCommand::IsMeaningfulChange(Before, After))
	{
		return;
	}
	Record(MakeShared<FStageParameterItemCommand>(Item->GetInstanceId(), Item->GetInstanceLabel(), ParameterName, ParameterId, Before, After));
}

bool UStageEditHistoryComponent::BeginTransaction()
{
	if (Transaction.IsSet())
	{
		return false;
	}
	Transaction.Emplace();
	return true;
}

void UStageEditHistoryComponent::EndTransaction()
{
	if (!Transaction.IsSet())
	{
		return;
	}
	const FPendingTransaction Collected = MoveTemp(Transaction.GetValue());
	Transaction.Reset();

	TArray<TSharedRef<IStageEditCommand>> Commands = MakeTransformCommands(Collected.Transforms);
	for (const FPendingParameterChange& Change : Collected.Parameters)
	{
		AModularBaseActor* Item = Change.Item.Get();
		FStageParameterValue After;
		if (IsValid(Item) && IStageParameterInterface::Execute_GetParameterValue(Item, Change.ParameterId, After)
			&& FStageParameterItemCommand::IsMeaningfulChange(Change.Before, After))
		{
			Commands.Add(MakeShared<FStageParameterItemCommand>(Item->GetInstanceId(), Item->GetInstanceLabel(), Change.ParameterName, Change.ParameterId, Change.Before, After));
		}
	}

	const int32 Count = Commands.Num();
	RecordAsOneStep(MoveTemp(Commands), FText::Format(NSLOCTEXT("StageEditHistory", "EditItems", "Edit {0} items"), FText::AsNumber(Count)));
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

	// A drag still open is a finished edit by now: it becomes a step first, so undo reverts it rather than an older one.
	EndTransaction();

	StageEditHistory::FLiveItemEditor Editor(*Session, *SpawnSystem, *Selection, ParameterApplier, TransformValidator);
	return bUndo ? History->Undo(Editor) : History->Redo(Editor);
}
