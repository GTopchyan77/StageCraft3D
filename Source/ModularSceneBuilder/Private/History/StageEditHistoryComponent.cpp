// Copyright Epic Games, Inc. All Rights Reserved.

#include "History/StageEditHistoryComponent.h"

#include "Actors/ModularBaseActor.h"
#include "Components/SelectionComponent.h"
#include "Components/SpawnSystemComponent.h"
#include "Data/BaseItemData.h"
#include "Engine/World.h"
#include "History/StageBatchCommand.h"
#include "History/StageEditHistorySubsystem.h"
#include "History/StageItemCommands.h"
#include "History/StageItemSnapshot.h"
#include "Interaction/StageParameterInterface.h"
#include "Interaction/StageTransformRules.h"
#include "ModularSceneBuilder.h"
#include "Placement/StagePlacementToolComponent.h"
#include "Placement/StageSpanToolComponent.h"
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
		FLiveItemEditor(UStageSessionSubsystem& InSession, USpawnSystemComponent& InSpawnSystem, USelectionComponent& InSelection, const FStageParameterApplier& InParameterApplier)
			: Session(InSession), SpawnSystem(InSpawnSystem), Selection(InSelection), ParameterApplier(InParameterApplier)
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

	private:
		UStageSessionSubsystem& Session;
		USpawnSystemComponent& SpawnSystem;
		USelectionComponent& Selection;
		const FStageParameterApplier& ParameterApplier;
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
	ParameterApplier.Unbind();
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

	const int32 Count = Commands.Num();
	RecordAsOneStep(MoveTemp(Commands), FText::Format(NSLOCTEXT("StageEditHistory", "TransformItems", "Transform {0} items"), FText::AsNumber(Count)));
}

void UStageEditHistoryComponent::RecordParameterChange(AActor* Target, const FGameplayTag& ParameterId, const FText& ParameterName, const FStageParameterValue& Before)
{
	AModularBaseActor* Item = Cast<AModularBaseActor>(Target);
	if (!IsValid(Item))
	{
		return;
	}

	FStageParameterValue After;
	if (!IStageParameterInterface::Execute_GetParameterValue(Item, ParameterId, After) || !FStageParameterItemCommand::IsMeaningfulChange(Before, After))
	{
		return;
	}
	Record(MakeShared<FStageParameterItemCommand>(Item->GetInstanceId(), Item->GetInstanceLabel(), ParameterName, ParameterId, Before, After));
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

	StageEditHistory::FLiveItemEditor Editor(*Session, *SpawnSystem, *Selection, ParameterApplier);
	return bUndo ? History->Undo(Editor) : History->Redo(Editor);
}
