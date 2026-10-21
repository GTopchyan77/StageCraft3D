// Copyright Epic Games, Inc. All Rights Reserved.

// Integration tests for undo/redo against a real game world (Docs/ADR/0010-edit-rules-autosave-scene-v2.md §2): the rules
// decide every undone or redone transform (a locked item stays put, a group step is all-or-nothing), damaged steps are
// dropped, and a continuous edit inside a transaction becomes exactly one step.
// Run: UnrealEditor-Cmd <project> -ExecCmds="Automation RunTests StageCraft.History; Quit" -unattended -nullrhi

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Actors/ModularBaseActor.h"
#include "Components/SelectionComponent.h"
#include "Components/SpawnSystemComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Game/StageCraftGameModeBase.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/WorldSettings.h"
#include "History/StageEditHistoryComponent.h"
#include "History/StageEditHistorySubsystem.h"
#include "Interaction/StageParameterInterface.h"
#include "Interaction/StageTransformRules.h"
#include "Misc/AutomationTest.h"

#include <limits>

namespace StageHistoryWorldTests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter;

	/**
	 * A game world that has begun play, with the pieces the history works against: the GameMode (rules authority), a player,
	 * and an actor carrying the spawn, selection and history components, its TransformValidator bound to the GameMode exactly
	 * as AModularPlayerController binds it. Destroyed with the scope.
	 */
	struct FScopedStage
	{
		UWorld* World = nullptr;
		AStageCraftGameModeBase* GameMode = nullptr;
		APlayerController* Player = nullptr;
		UStageEditHistoryComponent* EditHistory = nullptr;
		UStageEditHistorySubsystem* History = nullptr;

		FScopedStage()
		{
			World = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false, TEXT("StageHistoryTestWorld"));
			FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
			Context.SetCurrentWorld(World);
			World->InitializeActorsForPlay(FURL());
			World->BeginPlay();
			World->GetWorldSettings()->NotifyBeginPlay();

			// A sandbox session: this test world has no game instance, so no economy to ask about ownership. SessionRules is a
			// protected, defaults-only property; a test may set it through reflection, game code never does.
			GameMode = World->SpawnActor<AStageCraftGameModeBase>();
			if (const FStructProperty* RulesProperty = FindFProperty<FStructProperty>(AStageCraftGameModeBase::StaticClass(), TEXT("SessionRules")))
			{
				RulesProperty->ContainerPtrToValuePtr<FStageSessionRules>(GameMode)->bEnforceEntitlements = false;
			}
			Player = World->SpawnActor<APlayerController>();

			AActor* Owner = World->SpawnActor<AActor>();
			// Registered after the owner began play, so each component begins play on registration, in this order (the history
			// component finds the other two in its BeginPlay).
			for (UClass* ComponentClass : { USpawnSystemComponent::StaticClass(), USelectionComponent::StaticClass(), UStageEditHistoryComponent::StaticClass() })
			{
				UActorComponent* Component = NewObject<UActorComponent>(Owner, ComponentClass);
				Component->RegisterComponent();
			}
			EditHistory = Owner->FindComponentByClass<UStageEditHistoryComponent>();
			EditHistory->TransformValidator.BindLambda([this](const AModularBaseActor& Item, const FTransform& Transform)
			{
				return GameMode->EvaluateTransformChange(Player, Item, Transform).IsSuccess();
			});
			History = UWorld::GetSubsystem<UStageEditHistorySubsystem>(World);
		}

		~FScopedStage()
		{
			EditHistory->TransformValidator.Unbind();
			World->BeginTearingDown();
			World->EndPlay(EEndPlayReason::Quit);
			World->DestroyWorld(/*bInformEngineOfWorld*/ false);
			GEngine->DestroyWorldContext(World);
		}

		AModularBaseActor* SpawnItem(const FVector& Location) const
		{
			return World->SpawnActor<AModularBaseActor>(Location, FRotator::ZeroRotator);
		}

		static void SetLocked(AModularBaseActor& Item, bool bLocked)
		{
			IStageParameterInterface::Execute_SetParameterValue(&Item, StageCraftTags::Param_Info_Locked, FStageParameterValue::MakeBool(bLocked));
		}

		/** Moves Item to Location the way a finished gizmo drag does: write, then record the change as one step. */
		void MoveAndRecord(AModularBaseActor& Item, const FVector& Location) const
		{
			const FTransform Before = Item.GetActorTransform();
			Item.SetActorLocation(Location);
			EditHistory->RecordTransformChange(&Item, Before);
		}
	};

	FString Str(EStageCommandResult Result) { return UEnum::GetValueAsString(Result); }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageHistoryRulesTest, "StageCraft.History.Rules", StageHistoryWorldTests::Flags)

bool FStageHistoryRulesTest::RunTest(const FString& Parameters)
{
	using namespace StageHistoryWorldTests;

	FScopedStage Stage;
	if (!TestNotNull(TEXT("History component"), Stage.EditHistory) || !TestNotNull(TEXT("History subsystem"), Stage.History) || !TestNotNull(TEXT("Game mode"), Stage.GameMode))
	{
		return false;
	}

	const FVector Start(0.0, 0.0, 0.0);
	const FVector Moved(100.0, 0.0, 0.0);
	AModularBaseActor* Item = Stage.SpawnItem(Start);
	if (!TestNotNull(TEXT("Item"), Item))
	{
		return false;
	}

	// The GameMode judges a whole transform: unchanged parts are not judged, any changed part of a locked item is refused.
	{
		FScopedStage::SetLocked(*Item, true);
		TestTrue(TEXT("Rules: putting a locked item where it already is is allowed"), Stage.GameMode->EvaluateTransformChange(Stage.Player, *Item, Item->GetActorTransform()).IsSuccess());
		TestFalse(TEXT("Rules: moving a locked item is refused"), Stage.GameMode->EvaluateTransformChange(Stage.Player, *Item, FTransform(Moved)).IsSuccess());
		FTransform Turned = Item->GetActorTransform();
		Turned.SetRotation(FRotator(0.0, 90.0, 0.0).Quaternion());
		TestFalse(TEXT("Rules: turning a locked item is refused"), Stage.GameMode->EvaluateTransformChange(Stage.Player, *Item, Turned).IsSuccess());
		FScopedStage::SetLocked(*Item, false);
		TestTrue(TEXT("Rules: moving an unlocked item is allowed"), Stage.GameMode->EvaluateTransformChange(Stage.Player, *Item, FTransform(Moved)).IsSuccess());
		TestFalse(TEXT("Rules: a position 20 km away is refused"), Stage.GameMode->EvaluateTransformChange(Stage.Player, *Item, FTransform(FVector(2.0e6, 0.0, 0.0))).IsSuccess());
		FTransform NaN = FTransform::Identity;
		NaN.SetLocation(FVector(std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0));
		TestFalse(TEXT("Rules: a NaN position is refused"), Stage.GameMode->EvaluateTransformChange(Stage.Player, *Item, NaN).IsSuccess());
	}

	// Undo of a move on a locked item is refused and changes nothing; once unlocked, it applies.
	Stage.MoveAndRecord(*Item, Moved);
	TestEqual(TEXT("One step recorded"), Stage.History->GetUndoCount(), 1);
	FScopedStage::SetLocked(*Item, true);
	TestEqual(TEXT("Undo of a locked item is refused"), Str(Stage.EditHistory->Undo()), Str(EStageCommandResult::Refused));
	TestTrue(TEXT("... the item did not move"), Item->GetActorLocation().Equals(Moved));
	TestEqual(TEXT("... the step is still there to retry"), Stage.History->GetUndoCount(), 1);
	FScopedStage::SetLocked(*Item, false);
	TestEqual(TEXT("Undo once unlocked"), Str(Stage.EditHistory->Undo()), Str(EStageCommandResult::Succeeded));
	TestTrue(TEXT("... back at the start"), Item->GetActorLocation().Equals(Start));

	// Redo obeys the same rules.
	FScopedStage::SetLocked(*Item, true);
	TestEqual(TEXT("Redo of a locked item is refused"), Str(Stage.EditHistory->Redo()), Str(EStageCommandResult::Refused));
	TestTrue(TEXT("... the item did not move"), Item->GetActorLocation().Equals(Start));
	FScopedStage::SetLocked(*Item, false);
	TestEqual(TEXT("Redo once unlocked"), Str(Stage.EditHistory->Redo()), Str(EStageCommandResult::Succeeded));
	TestTrue(TEXT("... moved again"), Item->GetActorLocation().Equals(Moved));

	// A group step checks every item: one locked item refuses the whole step, and the items already undone are put back.
	AModularBaseActor* Other = Stage.SpawnItem(FVector(0.0, 200.0, 0.0));
	if (!TestNotNull(TEXT("Second item"), Other))
	{
		return false;
	}
	const FTransform ItemBefore = Item->GetActorTransform();
	const FTransform OtherBefore = Other->GetActorTransform();
	Item->SetActorLocation(Moved + FVector(0.0, 50.0, 0.0));
	Other->SetActorLocation(FVector(0.0, 250.0, 0.0));
	const FStageTransformChange GroupChanges[] = { { Item, ItemBefore }, { Other, OtherBefore } };
	const int32 StepsBeforeGroup = Stage.History->GetUndoCount();
	Stage.EditHistory->RecordTransformChanges(GroupChanges);
	TestEqual(TEXT("A group move is one step"), Stage.History->GetUndoCount(), StepsBeforeGroup + 1);

	// Undo walks a group step newest first, so Other is undone before Item refuses: Other must be rolled back.
	FScopedStage::SetLocked(*Item, true);
	TestEqual(TEXT("Group undo with one locked item is refused"), Str(Stage.EditHistory->Undo()), Str(EStageCommandResult::Refused));
	TestTrue(TEXT("... the item undone first was rolled back"), Other->GetActorLocation().Equals(FVector(0.0, 250.0, 0.0)));
	TestTrue(TEXT("... the locked item did not move"), Item->GetActorLocation().Equals(Moved + FVector(0.0, 50.0, 0.0)));
	FScopedStage::SetLocked(*Item, false);
	TestEqual(TEXT("Group undo once unlocked"), Str(Stage.EditHistory->Undo()), Str(EStageCommandResult::Succeeded));
	TestTrue(TEXT("... both back"), Item->GetActorTransform().Equals(ItemBefore) && Other->GetActorTransform().Equals(OtherBefore));

	// A step that can never apply (a damaged, out-of-range transform) is dropped instead of being offered forever.
	const FVector FarAway(5.0e6, 0.0, 0.0);
	Stage.MoveAndRecord(*Item, FarAway);
	TestEqual(TEXT("Undo back from far away"), Str(Stage.EditHistory->Undo()), Str(EStageCommandResult::Succeeded));
	const int32 RedoSteps = Stage.History->GetRedoCount();
	TestEqual(TEXT("Redo to an invalid position is dropped"), Str(Stage.EditHistory->Redo()), Str(EStageCommandResult::Invalid));
	TestEqual(TEXT("... and removed from the redo stack"), Stage.History->GetRedoCount(), RedoSteps - 1);
	TestTrue(TEXT("... the item stays where it was"), StageTransformRules::IsSaneTransform(Item->GetActorTransform()));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageHistoryTransactionTest, "StageCraft.History.Transaction", StageHistoryWorldTests::Flags)

bool FStageHistoryTransactionTest::RunTest(const FString& Parameters)
{
	using namespace StageHistoryWorldTests;

	FScopedStage Stage;
	if (!TestNotNull(TEXT("History component"), Stage.EditHistory) || !TestNotNull(TEXT("History subsystem"), Stage.History))
	{
		return false;
	}
	AModularBaseActor* Item = Stage.SpawnItem(FVector::ZeroVector);
	AModularBaseActor* Other = Stage.SpawnItem(FVector(0.0, 300.0, 0.0));
	if (!TestNotNull(TEXT("Items"), Item) || !TestNotNull(TEXT("Items"), Other))
	{
		return false;
	}

	// A slider drag: fifty value changes, each validated and applied and recorded as it happens, become one step.
	TestTrue(TEXT("A transaction opens"), Stage.EditHistory->BeginTransaction());
	TestFalse(TEXT("... only one at a time"), Stage.EditHistory->BeginTransaction());
	for (int32 Index = 1; Index <= 50; ++Index)
	{
		Stage.MoveAndRecord(*Item, FVector(Index * 10.0, 0.0, 0.0));
	}
	TestEqual(TEXT("Nothing is recorded while the drag runs"), Stage.History->GetUndoCount(), 0);
	Stage.EditHistory->EndTransaction();
	TestFalse(TEXT("The transaction closed"), Stage.EditHistory->IsTransactionOpen());
	TestEqual(TEXT("The whole drag is exactly one step"), Stage.History->GetUndoCount(), 1);
	TestEqual(TEXT("Undo"), Str(Stage.EditHistory->Undo()), Str(EStageCommandResult::Succeeded));
	TestTrue(TEXT("... goes back to before the drag, not one move back"), Item->GetActorLocation().Equals(FVector::ZeroVector));
	TestEqual(TEXT("Redo"), Str(Stage.EditHistory->Redo()), Str(EStageCommandResult::Succeeded));
	TestTrue(TEXT("... goes to where the drag ended"), Item->GetActorLocation().Equals(FVector(500.0, 0.0, 0.0)));

	// A group drag in a transaction: both items, one step.
	Stage.EditHistory->BeginTransaction();
	for (int32 Index = 1; Index <= 10; ++Index)
	{
		const FStageTransformChange Changes[] = { { Item, Item->GetActorTransform() }, { Other, Other->GetActorTransform() } };
		Item->AddActorWorldOffset(FVector(0.0, 0.0, 5.0));
		Other->AddActorWorldOffset(FVector(0.0, 0.0, 5.0));
		Stage.EditHistory->RecordTransformChanges(Changes);
	}
	Stage.EditHistory->EndTransaction();
	TestEqual(TEXT("A group drag is one step too"), Stage.History->GetUndoCount(), 2);
	Stage.EditHistory->Undo();
	TestEqual(TEXT("... undone together (first item)"), Item->GetActorLocation().Z, 0.0);
	TestEqual(TEXT("... undone together (second item)"), Other->GetActorLocation().Z, 0.0);

	// A drag that ends where it started records nothing.
	const int32 StepsBefore = Stage.History->GetUndoCount();
	Stage.EditHistory->BeginTransaction();
	Stage.MoveAndRecord(*Item, Item->GetActorLocation() + FVector(30.0, 0.0, 0.0));
	Stage.MoveAndRecord(*Item, Item->GetActorLocation() - FVector(30.0, 0.0, 0.0));
	Stage.EditHistory->EndTransaction();
	TestEqual(TEXT("A drag back to the start is no step"), Stage.History->GetUndoCount(), StepsBefore);

	// Undo while a drag is still open: the drag becomes its step first, so undo reverts the drag rather than an older step.
	const FVector BeforeOpenDrag = Item->GetActorLocation();
	Stage.EditHistory->BeginTransaction();
	Stage.MoveAndRecord(*Item, BeforeOpenDrag + FVector(0.0, 40.0, 0.0));
	TestEqual(TEXT("Undo during a drag"), Str(Stage.EditHistory->Undo()), Str(EStageCommandResult::Succeeded));
	TestFalse(TEXT("... closed the transaction"), Stage.EditHistory->IsTransactionOpen());
	TestTrue(TEXT("... and reverted the drag"), Item->GetActorLocation().Equals(BeforeOpenDrag));

	// Ending a transaction that is not open does nothing.
	const int32 StepsNow = Stage.History->GetUndoCount();
	Stage.EditHistory->EndTransaction();
	TestEqual(TEXT("A stray end records nothing"), Stage.History->GetUndoCount(), StepsNow);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
