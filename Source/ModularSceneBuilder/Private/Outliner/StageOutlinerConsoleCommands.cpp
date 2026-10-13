// Copyright Epic Games, Inc. All Rights Reserved.

// Development console commands for the outliner (Docs/ADR/0005-spans-outliner-and-bill-of-materials.md §4). They list the same
// model the Outliner panel shows and drive the same controller requests its rows use, so grouping, visibility, locking and group
// selection can be verified from logs. Targets: an actor name, an item label, or group:<Discipline>. Compiled out of Shipping.

#include "CoreMinimal.h"

#if !UE_BUILD_SHIPPING

#include "Actors/ModularBaseActor.h"
#include "Algo/AnyOf.h"
#include "Components/SelectionComponent.h"
#include "Data/BaseItemData.h"
#include "Data/StageParameterTypes.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "ModularSceneBuilder.h"
#include "Outliner/StageOutlinerModel.h"
#include "Player/ModularPlayerController.h"
#include "Subsystems/StageSessionSubsystem.h"

namespace StageOutlinerCommands
{
	const TCHAR* const GroupPrefix = TEXT("group:");

	AModularPlayerController* GetController(UWorld* World)
	{
		AModularPlayerController* Controller = World ? Cast<AModularPlayerController>(World->GetFirstPlayerController()) : nullptr;
		if (!Controller)
		{
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Outliner: no AModularPlayerController in this world."));
		}
		return Controller;
	}

	/** Items named by Target: group:<Discipline> (e.g. group:Lighting), or an actor name or label. */
	TArray<AModularBaseActor*> ResolveTargets(UWorld* World, const FString& Target)
	{
		TArray<AModularBaseActor*> Result;
		const UStageSessionSubsystem* Session = UWorld::GetSubsystem<UStageSessionSubsystem>(World);
		if (!Session)
		{
			return Result;
		}

		const bool bGroup = Target.StartsWith(GroupPrefix, ESearchCase::IgnoreCase);
		const FString Name = bGroup ? Target.RightChop(FCString::Strlen(GroupPrefix)) : Target;
		for (AModularBaseActor* Item : Session->GetPlacedItems())
		{
			const UBaseItemData* Data = Item->GetItemData();
			const bool bMatches = bGroup
				? StageOutliner::MakeGroupLabel(StageOutliner::GetGroupCategory(Data ? Data->CategoryTag : FGameplayTag())).ToString().Equals(Name, ESearchCase::IgnoreCase)
				: Item->GetName().Equals(Name, ESearchCase::IgnoreCase) || Item->GetInstanceLabel().ToString().Equals(Name, ESearchCase::IgnoreCase);
			if (bMatches)
			{
				Result.Add(Item);
			}
		}
		return Result;
	}

	void List(const TArray<FString>& Args, UWorld* World)
	{
		const UStageSessionSubsystem* Session = UWorld::GetSubsystem<UStageSessionSubsystem>(World);
		const AModularPlayerController* Controller = GetController(World);
		if (!Session || !Controller)
		{
			return;
		}

		TArray<FStageOutlinerItem> Items;
		TMap<FGuid, const AModularBaseActor*> Actors;
		for (const AModularBaseActor* Actor : Session->GetPlacedItems())
		{
			const UBaseItemData* Data = Actor->GetItemData();
			FStageOutlinerItem& Item = Items.AddDefaulted_GetRef();
			Item.InstanceId = Actor->GetInstanceId();
			Item.Label = Actor->GetInstanceLabel();
			Item.ModelName = Data ? Data->DisplayName : FText::GetEmpty();
			Item.Category = Data ? Data->CategoryTag : FGameplayTag();
			Item.Quantity = Actor->GetQuantity();
			Item.bHidden = Actor->IsHiddenOnStage();
			Item.bLocked = Actor->IsTransformLocked();
			Actors.Add(Item.InstanceId, Actor);
		}

		const auto FlagName = [](EStageOutlinerFlagState State)
		{
			return State == EStageOutlinerFlagState::All ? TEXT("all") : (State == EStageOutlinerFlagState::Some ? TEXT("some") : TEXT("none"));
		};
		for (const FStageOutlinerGroup& Group : StageOutliner::BuildGroups(MoveTemp(Items)))
		{
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Outliner: [%s] %d items (hidden %s, locked %s)"), *Group.Label.ToString(), Group.Items.Num(),
				FlagName(Group.Hidden), FlagName(Group.Locked));
			for (const FStageOutlinerItem& Item : Group.Items)
			{
				const AModularBaseActor* Actor = Actors.FindRef(Item.InstanceId);
				UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Outliner:     %s \"%s\" (%s) x%d%s%s%s"), *GetNameSafe(Actor), *Item.Label.ToString(), *Item.ModelName.ToString(),
					Item.Quantity, Item.bHidden ? TEXT(" HIDDEN") : TEXT(""), Item.bLocked ? TEXT(" LOCKED") : TEXT(""),
					Controller->GetSelection()->IsActorSelected(Actor) ? TEXT(" SELECTED") : TEXT(""));
			}
		}
	}

	/** Hide/Lock <Target> [on|off]: on hides or locks; without it the targets toggle like a click on the group's icon. */
	void SetFlag(const TArray<FString>& Args, UWorld* World, const FGameplayTag& ParameterId, const TCHAR* CommandName)
	{
		AModularPlayerController* Controller = GetController(World);
		const TArray<AModularBaseActor*> Targets = Args.IsValidIndex(0) ? ResolveTargets(World, Args[0]) : TArray<AModularBaseActor*>();
		if (!Controller || Targets.IsEmpty())
		{
			UE_LOG(LogStageCraft, Display, TEXT("Usage: %s <ActorName|Label|group:Discipline> [on|off]"), CommandName);
			return;
		}

		const bool bVisibility = ParameterId == StageCraftTags::Param_Info_Visible;
		const auto IsSet = [bVisibility](const AModularBaseActor* Item) { return bVisibility ? Item->IsHiddenOnStage() : Item->IsTransformLocked(); };
		const bool bTurnOn = Args.IsValidIndex(1) ? Args[1].Equals(TEXT("on"), ESearchCase::IgnoreCase) : !Algo::AnyOf(Targets, IsSet);

		int32 Changed = 0;
		for (AModularBaseActor* Item : Targets)
		{
			if (IsSet(Item) != bTurnOn)
			{
				// Visible is the inverse of hidden; Locked is the flag itself.
				const bool bValue = bVisibility ? !bTurnOn : bTurnOn;
				Changed += Controller->RequestParameterChange(Item, ParameterId, FStageParameterValue::MakeBool(bValue)).IsSuccess() ? 1 : 0;
			}
		}
		UE_LOG(LogStageCraft, Display, TEXT("%s %s %s: %d of %d items changed; selection now %d."), CommandName, *Args[0], bTurnOn ? TEXT("on") : TEXT("off"),
			Changed, Targets.Num(), Controller->GetSelection()->GetSelectionCount());
	}

	void Hide(const TArray<FString>& Args, UWorld* World)
	{
		SetFlag(Args, World, StageCraftTags::Param_Info_Visible, TEXT("StageCraft.Outliner.Hide"));
	}

	void Lock(const TArray<FString>& Args, UWorld* World)
	{
		SetFlag(Args, World, StageCraftTags::Param_Info_Locked, TEXT("StageCraft.Outliner.Lock"));
	}

	void Select(const TArray<FString>& Args, UWorld* World)
	{
		AModularPlayerController* Controller = GetController(World);
		const TArray<AModularBaseActor*> Targets = Args.IsValidIndex(0) ? ResolveTargets(World, Args[0]) : TArray<AModularBaseActor*>();
		if (!Controller || Targets.IsEmpty())
		{
			UE_LOG(LogStageCraft, Display, TEXT("Usage: StageCraft.Outliner.Select <ActorName|Label|group:Discipline> [ctrl]"));
			return;
		}

		const bool bCtrl = Args.IsValidIndex(1) && Args[1].Equals(TEXT("ctrl"), ESearchCase::IgnoreCase);
		Controller->RequestSelectItems(Targets, bCtrl ? EStageSelectionClick::Toggle : EStageSelectionClick::Replace);
		UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Outliner.Select %s%s: %d selected, primary %s."), *Args[0], bCtrl ? TEXT(" (ctrl)") : TEXT(""),
			Controller->GetSelection()->GetSelectionCount(), *GetNameSafe(Controller->GetSelection()->GetSelectedActor()));
	}

	FAutoConsoleCommandWithWorldAndArgs ListCommand(TEXT("StageCraft.Outliner.List"), TEXT("Logs the outliner's groups and items with hidden / locked / selected flags."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&List));
	FAutoConsoleCommandWithWorldAndArgs HideCommand(TEXT("StageCraft.Outliner.Hide"), TEXT("<ActorName|Label|group:Discipline> [on|off] Hides or shows, like the outliner's eye."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Hide));
	FAutoConsoleCommandWithWorldAndArgs LockCommand(TEXT("StageCraft.Outliner.Lock"), TEXT("<ActorName|Label|group:Discipline> [on|off] Locks or unlocks, like the outliner's lock."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Lock));
	FAutoConsoleCommandWithWorldAndArgs SelectCommand(TEXT("StageCraft.Outliner.Select"), TEXT("<ActorName|Label|group:Discipline> [ctrl] Selects like a click on an outliner row."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Select));
}

#endif // !UE_BUILD_SHIPPING
