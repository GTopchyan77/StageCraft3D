// Copyright Epic Games, Inc. All Rights Reserved.

// Integration test for Sketch to Stage against a real game world (Docs/ADR/0015-sketch-to-stage.md §2.5-2.6): a plan is placed through
// a spawn system (decks, a truss path, fixtures), recorded as ONE undo step, undone and redone as a whole; a refused element
// leaves the stage exactly as it was; decks rebuild on a height edit and repair a damaged outline.
// Run: UnrealEditor-Cmd <project> -ExecCmds="Automation RunTests StageCraft.Sketch; Quit" -unattended -nullrhi

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Actors/ModularBaseActor.h"
#include "Actors/StageDeckActor.h"
#include "Actors/StageSpanActor.h"
#include "Components/SelectionComponent.h"
#include "Components/SpawnSystemComponent.h"
#include "Data/StageDeckData.h"
#include "Data/StageSpanData.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/WorldSettings.h"
#include "History/StageEditHistoryComponent.h"
#include "History/StageEditHistorySubsystem.h"
#include "Interaction/StageParameterInterface.h"
#include "Misc/AutomationTest.h"
#include "Sketch/StageSketchBuilder.h"
#include "Sketch/StageSketchGenerator.h"
#include "Sketch/StageSketchMath.h"
#include "Sketch/StageSketchSubsystem.h"
#include "Subsystems/StageSessionSubsystem.h"
#include "UObject/Package.h"

namespace StageSketchWorldTests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter;

	/** A game world that has begun play, with an actor carrying the spawn, selection and history components. Destroyed with the scope. */
	struct FScopedStage
	{
		UWorld* World = nullptr;
		USpawnSystemComponent* Spawner = nullptr;
		UStageEditHistoryComponent* EditHistory = nullptr;
		UStageSessionSubsystem* Session = nullptr;

		FScopedStage()
		{
			World = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false, TEXT("StageSketchTestWorld"));
			FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
			Context.SetCurrentWorld(World);
			World->InitializeActorsForPlay(FURL());
			World->BeginPlay();
			World->GetWorldSettings()->NotifyBeginPlay();

			AActor* Owner = World->SpawnActor<AActor>();
			for (UClass* ComponentClass : { USpawnSystemComponent::StaticClass(), USelectionComponent::StaticClass(), UStageEditHistoryComponent::StaticClass() })
			{
				UActorComponent* Component = NewObject<UActorComponent>(Owner, ComponentClass);
				Component->RegisterComponent();
			}
			Spawner = Owner->FindComponentByClass<USpawnSystemComponent>();
			EditHistory = Owner->FindComponentByClass<UStageEditHistoryComponent>();
			Session = UWorld::GetSubsystem<UStageSessionSubsystem>(World);
		}

		~FScopedStage()
		{
			World->BeginTearingDown();
			World->EndPlay(EEndPlayReason::Quit);
			World->DestroyWorld(/*bInformEngineOfWorld*/ false);
			GEngine->DestroyWorldContext(World);
		}

		int32 CountPlaced() const { return Session ? Session->GetPlacedItems().Num() : 0; }

		template <typename T>
		T* FindFirst() const
		{
			for (AModularBaseActor* Item : Session->GetPlacedItems())
			{
				if (T* Typed = Cast<T>(Item))
				{
					return Typed;
				}
			}
			return nullptr;
		}
	};

	FStageSketchStroke MakeStroke(EStageSketchStrokeKind Kind, TArray<FVector2D> Points, bool bClosed = false)
	{
		FStageSketchStroke Stroke;
		Stroke.Kind = Kind;
		Stroke.Points = MoveTemp(Points);
		Stroke.bClosed = bClosed;
		Stroke.DeckHeight = 80.f;
		return Stroke;
	}

	FStageEconomyResultInfo Allow(const UBaseItemData&, int32)
	{
		return FStageEconomyResultInfo::Ok();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageSketchGenerateTest, "StageCraft.Sketch.Generate", StageSketchWorldTests::Flags)

bool FStageSketchGenerateTest::RunTest(const FString& Parameters)
{
	using namespace StageSketchWorldTests;

	FScopedStage Stage;
	if (!TestNotNull(TEXT("Spawn system"), Stage.Spawner) || !TestNotNull(TEXT("History"), Stage.EditHistory) || !TestNotNull(TEXT("Session"), Stage.Session))
	{
		return false;
	}

	UStageDeckData* Deck = NewObject<UStageDeckData>(GetTransientPackage());
	Deck->DisplayName = FText::FromString(TEXT("Test Deck"));
	UStageSpanData* Truss = NewObject<UStageSpanData>(GetTransientPackage());
	Truss->DisplayName = FText::FromString(TEXT("Test Truss"));
	Truss->SegmentLength = 200.f;
	UBaseItemData* Fixture = NewObject<UBaseItemData>(GetTransientPackage());
	Fixture->DisplayName = FText::FromString(TEXT("Test Fixture"));
	Fixture->ActorClass = AModularBaseActor::StaticClass();

	FStageSketchBuildRules Rules;
	Rules.DeckItem = Deck;
	Rules.TrussItem = Truss;
	Rules.FixtureItem = Fixture;
	const FStageSketchBuildPlan Plan = StageSketchGenerator::BuildPlan({
		MakeStroke(EStageSketchStrokeKind::Shape, { FVector2D(0, 0), FVector2D(400, 0), FVector2D(400, 200), FVector2D(0, 200) }, true),
		MakeStroke(EStageSketchStrokeKind::Truss, { FVector2D(0, -600), FVector2D(800, -600) }),
		MakeStroke(EStageSketchStrokeKind::Fixture, { FVector2D(400, -590) }),
		MakeStroke(EStageSketchStrokeKind::Fixture, { FVector2D(100, 100) }) }, Rules);
	if (!TestEqual(TEXT("Plan: 1 deck, 1 truss, 2 fixtures"), Plan.Num(), 4))
	{
		return false;
	}

	// Placed through the spawn system, all of it.
	const FStageSketchBuildOutcome Outcome = StageSketchBuilder::Execute(Plan, *Stage.Spawner, &Allow);
	TestTrue(TEXT("Build succeeded"), Outcome.IsSuccess());
	TestEqual(TEXT("Four items placed"), Outcome.Placed.Num(), 4);
	TestEqual(TEXT("...and registered with the session"), Stage.CountPlaced(), 4);
	TestTrue(TEXT("...counted by kind"), Outcome.DeckCount == 1 && Outcome.TrussCount == 1 && Outcome.FixtureCount == 2);

	AStageDeckActor* DeckActor = Stage.FindFirst<AStageDeckActor>();
	if (TestNotNull(TEXT("A deck actor"), DeckActor))
	{
		TestEqual(TEXT("Deck: the drawn 4 x 2 m outline"), DeckActor->GetDeckArea(), 80000.0, 1e-6);
		TestEqual(TEXT("Deck: its height"), DeckActor->GetDeckHeight(), 80.0);
		TestEqual(TEXT("Deck: 4 panels of 2 m2 (its quantity)"), DeckActor->GetQuantity(), 4);
		TestEqual(TEXT("Deck: top (2) and walls (8) built"), DeckActor->GetMeshTriangleCount(), 10);
		TestTrue(TEXT("Deck: origin at the centroid on the floor"), DeckActor->GetActorLocation().Equals(FVector(200, 100, 0)));
		TestFalse(TEXT("Deck: does not scale"), DeckActor->SupportsScaling());

		// Height is a parameter: clamped to the item, rebuilt, read back.
		IStageParameterInterface::Execute_SetParameterValue(DeckActor, StageCraftTags::Param_Deck_Height, FStageParameterValue::MakeFloat(1000.0));
		FStageParameterValue Height;
		IStageParameterInterface::Execute_GetParameterValue(DeckActor, StageCraftTags::Param_Deck_Height, Height);
		TestEqual(TEXT("Deck: a height above the item's range is clamped"), Height.Float, double(Deck->GetMaxHeight()));
		TestTrue(TEXT("Deck height is an undoable shape parameter"), StageCraftTags::IsShapeParameter(StageCraftTags::Param_Deck_Height));
		IStageParameterInterface::Execute_SetParameterValue(DeckActor, StageCraftTags::Param_Deck_Height, FStageParameterValue::MakeFloat(80.0));
	}
	if (AStageSpanActor* Run = Stage.FindFirst<AStageSpanActor>())
	{
		TestEqual(TEXT("Truss: 8 m in 4 sections"), Run->GetQuantity(), 4);
		TestTrue(TEXT("Truss: at the truss height"), FMath::IsNearlyEqual(Run->GetActorLocation().Z, 600.0));
	}
	else
	{
		AddError(TEXT("No truss run placed"));
	}

	// One step for the whole generation.
	UStageEditHistorySubsystem* History = Stage.EditHistory->GetHistory();
	Stage.EditHistory->RecordPlacements(Outcome.Placed, FText::FromString(TEXT("Generate from Sketch (4 items)")));
	TestTrue(TEXT("Undo removes everything in one step"), Stage.EditHistory->Undo() == EStageCommandResult::Succeeded);
	TestEqual(TEXT("...the stage is empty"), Stage.CountPlaced(), 0);
	TestTrue(TEXT("A second undo has nothing to do"), Stage.EditHistory->Undo() == EStageCommandResult::NothingToDo);
	TestTrue(TEXT("Redo brings it all back"), Stage.EditHistory->Redo() == EStageCommandResult::Succeeded);
	TestEqual(TEXT("...four items"), Stage.CountPlaced(), 4);
	if (const AStageDeckActor* Restored = Stage.FindFirst<AStageDeckActor>())
	{
		TestEqual(TEXT("...the deck keeps its outline"), Restored->GetDeckArea(), 80000.0, 1e-6);
		TestEqual(TEXT("...and its height"), Restored->GetDeckHeight(), 80.0);
		TestEqual(TEXT("...with no repairs"), Restored->GetStateRepairCount(), 0);
	}
	TestNotNull(TEXT("History subsystem"), History);

	// All or nothing: the last fixture is refused, so nothing from this build stays.
	int32 Checked = 0;
	const FStageSketchBuildOutcome Refused = StageSketchBuilder::Execute(Plan, *Stage.Spawner, [&Checked, Fixture](const UBaseItemData& Item, int32 Quantity)
	{
		++Checked;
		return &Item == Fixture && Checked == 4
			? FStageEconomyResultInfo::Fail(EStageEconomyResult::SessionRuleViolation, FText::FromString(TEXT("Stage limit reached.")))
			: FStageEconomyResultInfo::Ok();
	});
	TestFalse(TEXT("A refused element fails the build"), Refused.IsSuccess());
	TestEqual(TEXT("...with the rule's reason"), Refused.Result.Message.ToString(), FString(TEXT("Stage limit reached.")));
	TestTrue(TEXT("...and nothing in the outcome"), Refused.Placed.IsEmpty() && Refused.DeckCount == 0);
	TestEqual(TEXT("...the stage is as it was"), Stage.CountPlaced(), 4);

	// A plan whose item went away is refused before anything spawns.
	FStageSketchBuildPlan Orphan = Plan;
	Orphan.FixtureItem = nullptr;
	TestFalse(TEXT("A missing item refuses the build"), StageSketchBuilder::Execute(Orphan, *Stage.Spawner, &Allow).IsSuccess());
	TestEqual(TEXT("...without spawning"), Stage.CountPlaced(), 4);

	// A damaged outline (a bow tie, as a hand-edited scene file could hold) becomes the default footprint, counted as a repair.
	AModularBaseActor* Damaged = Stage.Spawner->SpawnConfiguredItem(Deck, FTransform(FVector(0, 2000, 0)), [](AModularBaseActor& Actor)
	{
		if (AStageDeckActor* DeckActor = Cast<AStageDeckActor>(&Actor))
		{
			DeckActor->InitializeShape({ FVector2D(0, 0), FVector2D(200, 200), FVector2D(200, 0), FVector2D(0, 200) }, 60.0);
		}
	});
	if (const AStageDeckActor* Repaired = Cast<AStageDeckActor>(Damaged))
	{
		TestEqual(TEXT("Damaged deck: the default 2 x 1 m footprint"), Repaired->GetDeckArea(), 20000.0, 1e-6);
		TestTrue(TEXT("...counted as a repair"), Repaired->GetStateRepairCount() >= 1);
		TestTrue(TEXT("...and still built"), Repaired->GetMeshTriangleCount() > 0);
	}
	else
	{
		AddError(TEXT("The damaged deck did not spawn"));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageSketchSceneStateTest, "StageCraft.Sketch.SceneState", StageSketchWorldTests::Flags)

/** The sketch is part of the stage (format version 13, Docs/ADR/0015 §5, STATE.md #113): drawing is an unsaved change, and a loaded scene's sketch replaces the one drawn. */
bool FStageSketchSceneStateTest::RunTest(const FString& Parameters)
{
	using namespace StageSketchWorldTests;

	FScopedStage Stage;
	UStageSketchSubsystem* Sketch = UWorld::GetSubsystem<UStageSketchSubsystem>(Stage.World);
	if (!TestNotNull(TEXT("Session"), Stage.Session) || !TestNotNull(TEXT("Sketch"), Sketch))
	{
		return false;
	}

	// Unsaved changes: every sketch edit during play marks the stage dirty, so autosave and the exit prompt cover it.
	Stage.Session->MarkClean();
	TestTrue(TEXT("A stroke is added"), Sketch->AddStroke(MakeStroke(EStageSketchStrokeKind::Shape, { { 0.0, 0.0 }, { 600.0, 0.0 }, { 600.0, 400.0 }, { 0.0, 400.0 } })));
	TestTrue(TEXT("Drawing marks the stage unsaved"), Stage.Session->IsDirty());
	Stage.Session->MarkClean();
	FStageSketchSettings Settings = Sketch->GetSettings();
	Settings.TrussHeight = 800.f;
	Sketch->SetSettings(Settings);
	TestTrue(TEXT("A settings change marks the stage unsaved"), Stage.Session->IsDirty());
	Stage.Session->MarkClean();
	Sketch->SetSettings(Settings);
	TestFalse(TEXT("Setting the same settings is no change"), Stage.Session->IsDirty());
	Stage.Session->MarkClean();
	TestTrue(TEXT("Undo stroke"), Sketch->RemoveLastStroke());
	TestTrue(TEXT("Removing a stroke marks the stage unsaved"), Stage.Session->IsDirty());

	// Capture and restore: a scene's sketch comes back exactly; the one drawn before is replaced.
	Sketch->AddStroke(MakeStroke(EStageSketchStrokeKind::Freehand, { { 0.0, 0.0 }, { 0.0, 800.0 }, { 600.0, 800.0 }, { 600.0, 0.0 }, { 10.0, 10.0 } }));
	Sketch->AddStroke(MakeStroke(EStageSketchStrokeKind::Truss, { { -100.0, -100.0 }, { 700.0, -100.0 } }));
	Sketch->AddStroke(MakeStroke(EStageSketchStrokeKind::Fixture, { { 300.0, 200.0 } }));
	const FStageSketchSceneState Captured = Sketch->CaptureSceneState();
	if (!TestEqual(TEXT("Three strokes captured"), Captured.Strokes.Num(), 3) || !TestTrue(TEXT("The freehand gesture closed into a deck"), Captured.Strokes[0].bClosed))
	{
		return false;
	}
	TestEqual(TEXT("One item slot per element"), Captured.Items.Num(), int32(EStageSketchElement::Count));
	TestTrue(TEXT("Settings captured"), Captured.Settings == Settings);

	Sketch->ClearStrokes();
	Sketch->SetSettings(FStageSketchSettings());
	TestEqual(TEXT("Everything restored"), Sketch->RestoreSceneState(Captured), 0);
	if (TestEqual(TEXT("Strokes back"), Sketch->GetStrokes().Num(), 3))
	{
		for (int32 Index = 0; Index < 3; ++Index)
		{
			TestTrue(FString::Printf(TEXT("Stroke %d exactly as captured"), Index), Sketch->GetStrokes()[Index].Points == Captured.Strokes[Index].Points
				&& Sketch->GetStrokes()[Index].bClosed == Captured.Strokes[Index].bClosed && Sketch->GetStrokes()[Index].Kind == Captured.Strokes[Index].Kind);
		}
		TestEqual(TEXT("Still one deck"), Sketch->CountStrokes(EStageSketchElement::Deck), 1);
	}
	TestTrue(TEXT("Settings back"), Sketch->GetSettings() == Settings);

	// Deny by default: an item that is not a candidate of its element here is not chosen; the default builds it.
	FStageSketchSceneState Foreign = Captured;
	Foreign.Items[int32(EStageSketchElement::Deck)] = FSoftObjectPath(TEXT("/Game/StageCraft/Items/DA_Missing.DA_Missing"));
	TestEqual(TEXT("The unknown item is reported"), Sketch->RestoreSceneState(Foreign), 1);
	const TArray<UBaseItemData*> DeckCandidates = Sketch->GetCandidateItems(EStageSketchElement::Deck);
	TestTrue(TEXT("... and the first candidate builds decks"), Sketch->GetItem(EStageSketchElement::Deck) == (DeckCandidates.IsEmpty() ? nullptr : DeckCandidates[0]));
	TestTrue(TEXT("... and nothing is chosen"), Sketch->CaptureSceneState().Items[int32(EStageSketchElement::Deck)].IsNull());

	// Damaged strokes and the stroke limit, as a hand-edited state could hold them.
	FStageSketchSceneState Damaged;
	Damaged.Strokes.Add(MakeStroke(EStageSketchStrokeKind::Shape, { { 0.0, 0.0 }, { 100.0, 0.0 } }, true));
	for (int32 Index = 0; Index < StageSketchMath::MaxStrokes + 2; ++Index)
	{
		Damaged.Strokes.Add(MakeStroke(EStageSketchStrokeKind::Fixture, { { Index * 10.0, 0.0 } }));
	}
	TestEqual(TEXT("A two-corner shape and the strokes over the limit are not restored"), Sketch->RestoreSceneState(Damaged), 3);
	TestEqual(TEXT("... the sketch is full"), Sketch->GetStrokes().Num(), StageSketchMath::MaxStrokes);

	// A scene without a sketch (an older file, a starter stage) leaves an empty sketch with the default settings.
	Stage.Session->MarkClean();
	TestEqual(TEXT("An empty state restores"), Sketch->RestoreSceneState(FStageSketchSceneState()), 0);
	TestTrue(TEXT("... to an empty sketch"), Sketch->GetStrokes().IsEmpty());
	TestTrue(TEXT("... with the default settings"), Sketch->GetSettings() == FStageSketchSettings());
	TestTrue(TEXT("... and it is captured as empty"), Sketch->CaptureSceneState().IsEmpty());
	TestTrue(TEXT("Replacing the sketch during play is a change (a load marks the stage clean or unsaved afterwards)"), Stage.Session->IsDirty());
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
