// Copyright Epic Games, Inc. All Rights Reserved.

// Integration test for the assistant's stage shapes and replacements against a real game world (Docs/ADR/0018 §5-6): stairs, a
// curved truss and a round riser placed through a spawn system as ONE undo step; their parameters clamp, rebuild and predict
// their quantity; damaged saved sizes are repaired and counted; a replacement is one undo step that brings the old item back.
// Run: UnrealEditor-Cmd <project> -ExecCmds="Automation RunTests StageCraft.Shapes; Quit" -unattended -nullrhi

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Actors/ModularBaseActor.h"
#include "Actors/StageArcTrussActor.h"
#include "Actors/StageDeckActor.h"
#include "Actors/StageStairsActor.h"
#include "Components/SelectionComponent.h"
#include "Components/SpawnSystemComponent.h"
#include "Data/StageArcTrussData.h"
#include "Data/StageDeckData.h"
#include "Data/StageStairsData.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/WorldSettings.h"
#include "History/StageEditHistoryComponent.h"
#include "History/StageItemSnapshot.h"
#include "Interaction/StageParameterInterface.h"
#include "Misc/AutomationTest.h"
#include "Placement/StageDeckMath.h"
#include "Placement/StageProceduralShapes.h"
#include "Sketch/StageSketchBuilder.h"
#include "Subsystems/StageSessionSubsystem.h"
#include "UObject/Package.h"

#include <limits>

namespace StageShapeWorldTests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter;

	/** A game world that has begun play, with an actor carrying the spawn, selection and history components. Destroyed with the scope. */
	struct FScopedShapeStage
	{
		UWorld* World = nullptr;
		USpawnSystemComponent* Spawner = nullptr;
		USelectionComponent* Selection = nullptr;
		UStageEditHistoryComponent* EditHistory = nullptr;
		UStageSessionSubsystem* Session = nullptr;

		FScopedShapeStage()
		{
			World = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false, TEXT("StageShapeTestWorld"));
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
			Selection = Owner->FindComponentByClass<USelectionComponent>();
			EditHistory = Owner->FindComponentByClass<UStageEditHistoryComponent>();
			Session = UWorld::GetSubsystem<UStageSessionSubsystem>(World);
		}

		~FScopedShapeStage()
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

	FStageEconomyResultInfo Allow(const UBaseItemData&, int32)
	{
		return FStageEconomyResultInfo::Ok();
	}

	double ReadFloat(UObject* Item, const FGameplayTag& Parameter)
	{
		FStageParameterValue Value;
		IStageParameterInterface::Execute_GetParameterValue(Item, Parameter, Value);
		return Value.Float;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageShapeWorldTest, "StageCraft.Shapes.World", StageShapeWorldTests::Flags)

bool FStageShapeWorldTest::RunTest(const FString& Parameters)
{
	using namespace StageShapeWorldTests;

	FScopedShapeStage Stage;
	if (!TestNotNull(TEXT("Spawn system"), Stage.Spawner) || !TestNotNull(TEXT("History"), Stage.EditHistory) || !TestNotNull(TEXT("Session"), Stage.Session))
	{
		return false;
	}

	UStageStairsData* StairsItem = NewObject<UStageStairsData>(GetTransientPackage());
	StairsItem->DisplayName = FText::FromString(TEXT("Test Stairs"));
	UStageArcTrussData* ArcItem = NewObject<UStageArcTrussData>(GetTransientPackage());
	ArcItem->DisplayName = FText::FromString(TEXT("Test Arc"));
	UStageDeckData* DeckItem = NewObject<UStageDeckData>(GetTransientPackage());
	DeckItem->DisplayName = FText::FromString(TEXT("Test Deck"));

	FStageSketchBuildPlan Plan;
	Plan.StairsItem = StairsItem;
	Plan.ArcTrussItem = ArcItem;
	Plan.DeckItem = DeckItem;
	FStageSketchStairsPlan& Stairs = Plan.Stairs.AddDefaulted_GetRef();
	Stairs.Transform = FTransform(FVector(-500.0, 0.0, 0.0));
	Stairs.Rise = 60.0;
	Stairs.Width = 150.0;
	Stairs.bHandrails = false;
	FStageSketchArcTrussPlan& Arc = Plan.ArcTrusses.AddDefaulted_GetRef();
	Arc.Transform = FTransform(FVector(0.0, 400.0, 600.0));
	Arc.Radius = 400.0;
	Arc.AngleDegrees = 360.0;
	Arc.Sections = StageProceduralShapes::LayoutArcTruss(400.0, 360.0, ArcItem->ProfileWidth, ArcItem->SectionLength).Sections;
	FStageSketchDeckPlan& Round = Plan.Decks.AddDefaulted_GetRef();
	Round.Transform = FTransform(FVector(800.0, 0.0, 0.0));
	Round.LocalOutline = StageProceduralShapes::MakeCircle(200.0);
	Round.Height = 60.0;
	Round.Area = FMath::Abs(StageDeckMath::GetSignedArea(Round.LocalOutline));
	Round.Panels = StageDeckMath::GetPanelCount(Round.Area, DeckItem->GetPanelArea());

	const FStageSketchBuildOutcome Outcome = StageSketchBuilder::Execute(Plan, *Stage.Spawner, &Allow);
	TestTrue(TEXT("Build succeeded"), Outcome.IsSuccess());
	TestTrue(TEXT("...counted by kind"), Outcome.StairsCount == 1 && Outcome.ArcTrussCount == 1 && Outcome.DeckCount == 1);
	TestEqual(TEXT("...registered with the session"), Stage.CountPlaced(), 3);

	// Stairs: as planned, built, parameters clamp and rebuild.
	if (AStageStairsActor* StairsActor = Stage.FindFirst<AStageStairsActor>())
	{
		TestTrue(TEXT("Stairs: planned size"), FMath::IsNearlyEqual(StairsActor->GetRise(), 60.0) && FMath::IsNearlyEqual(StairsActor->GetWidth(), 150.0));
		TestFalse(TEXT("Stairs: no handrails"), StairsActor->HasHandrails());
		TestEqual(TEXT("Stairs: 3 steps of 20 cm"), StairsActor->GetLayout().Steps, 3);
		TestTrue(TEXT("Stairs: built"), StairsActor->GetMeshTriangleCount() > 0);
		TestEqual(TEXT("Stairs: one item"), StairsActor->GetQuantity(), 1);
		TestFalse(TEXT("Stairs: do not scale"), StairsActor->SupportsScaling());
		const int32 Plain = StairsActor->GetMeshTriangleCount();
		IStageParameterInterface::Execute_SetParameterValue(StairsActor, StageCraftTags::Param_Stairs_Handrails, FStageParameterValue::MakeBool(true));
		TestTrue(TEXT("Stairs: handrails add triangles"), StairsActor->GetMeshTriangleCount() > Plain);
		IStageParameterInterface::Execute_SetParameterValue(StairsActor, StageCraftTags::Param_Stairs_Rise, FStageParameterValue::MakeFloat(10000.0));
		TestEqual(TEXT("Stairs: rise clamped to the item"), ReadFloat(StairsActor, StageCraftTags::Param_Stairs_Rise), double(StairsItem->GetMaxRise()));
		TestTrue(TEXT("Stairs: more steps for more rise"), StairsActor->GetLayout().Steps > 3);
		TestFalse(TEXT("Stairs: a wrong type is refused"), IStageParameterInterface::Execute_SetParameterValue(StairsActor, StageCraftTags::Param_Stairs_Rise,
			FStageParameterValue::MakeBool(true)));
		TestTrue(TEXT("Stairs rise is an undoable shape parameter"), StageCraftTags::IsShapeParameter(StageCraftTags::Param_Stairs_Rise));
	}
	else
	{
		AddError(TEXT("No stairs placed"));
	}

	// Curved truss: a circle counted in rental sections, radius and angle predict and change the quantity.
	if (AStageArcTrussActor* ArcActor = Stage.FindFirst<AStageArcTrussActor>())
	{
		TestTrue(TEXT("Arc: a full circle"), ArcActor->GetLayout().IsFullCircle());
		TestEqual(TEXT("Arc: quantity is its sections"), ArcActor->GetQuantity(), Arc.Sections);
		TestTrue(TEXT("Arc: built"), ArcActor->GetMeshTriangleCount() > 0);
		const int32 Predicted = ArcActor->PredictQuantity(StageCraftTags::Param_Arc_Angle, FStageParameterValue::MakeFloat(180.0));
		TestTrue(TEXT("Arc: half the angle predicts fewer sections"), Predicted < ArcActor->GetQuantity());
		IStageParameterInterface::Execute_SetParameterValue(ArcActor, StageCraftTags::Param_Arc_Angle, FStageParameterValue::MakeFloat(180.0));
		TestEqual(TEXT("Arc: the prediction was right"), ArcActor->GetQuantity(), Predicted);
		IStageParameterInterface::Execute_SetParameterValue(ArcActor, StageCraftTags::Param_Arc_Radius, FStageParameterValue::MakeFloat(1.0));
		TestEqual(TEXT("Arc: radius clamped to the item"), ReadFloat(ArcActor, StageCraftTags::Param_Arc_Radius), double(ArcItem->GetMinRadius()));
		TestTrue(TEXT("Arc radius changes the quantity"), StageCraftTags::IsQuantityParameter(StageCraftTags::Param_Arc_Radius));
	}
	else
	{
		AddError(TEXT("No curved truss placed"));
	}

	if (const AStageDeckActor* RoundActor = Stage.FindFirst<AStageDeckActor>())
	{
		TestEqual(TEXT("Round riser: the circle's area"), RoundActor->GetDeckArea(), Round.Area, 1.0);
		TestEqual(TEXT("Round riser: no repairs"), RoundActor->GetStateRepairCount(), 0);
	}

	// One step for the whole build, undone and redone. The snapshots are the items as they are now (the edits above included).
	Stage.EditHistory->RecordPlacements(Outcome.Placed, FText::FromString(TEXT("Assistant: build")));
	TestTrue(TEXT("Undo removes the shapes"), Stage.EditHistory->Undo() == EStageCommandResult::Succeeded && Stage.CountPlaced() == 0);
	TestTrue(TEXT("Redo brings them back"), Stage.EditHistory->Redo() == EStageCommandResult::Succeeded && Stage.CountPlaced() == 3);
	if (const AStageStairsActor* Restored = Stage.FindFirst<AStageStairsActor>())
	{
		TestTrue(TEXT("...the stairs keep their edited size"), FMath::IsNearlyEqual(Restored->GetRise(), double(StairsItem->GetMaxRise()))
			&& FMath::IsNearlyEqual(Restored->GetWidth(), 150.0) && Restored->HasHandrails());
		TestEqual(TEXT("...with no repairs"), Restored->GetStateRepairCount(), 0);
	}

	// Damaged sizes (as a hand-edited scene file could hold) are clamped or defaulted and counted.
	AModularBaseActor* Damaged = Stage.Spawner->SpawnConfiguredItem(ArcItem, FTransform(FVector(0.0, 3000.0, 600.0)), [](AModularBaseActor& Actor)
	{
		if (AStageArcTrussActor* ArcActor = Cast<AStageArcTrussActor>(&Actor))
		{
			ArcActor->InitializeArc(std::numeric_limits<double>::quiet_NaN(), 1.0e9);
		}
	});
	if (const AStageArcTrussActor* Repaired = Cast<AStageArcTrussActor>(Damaged))
	{
		TestEqual(TEXT("Damaged arc: default radius"), Repaired->GetRadius(), double(ArcItem->GetDefaultRadius()));
		TestEqual(TEXT("Damaged arc: angle clamped"), Repaired->GetAngleDegrees(), double(ArcItem->GetMaxAngle()));
		TestTrue(TEXT("...counted as repairs"), Repaired->GetStateRepairCount() >= 2);
	}
	else
	{
		AddError(TEXT("The damaged arc did not spawn"));
	}

	// A fresh Library placement takes the item's defaults, without a repair.
	if (const AStageStairsActor* Fresh = Cast<AStageStairsActor>(Stage.Spawner->SpawnItem(StairsItem, FTransform(FVector(0.0, -3000.0, 0.0)))))
	{
		TestTrue(TEXT("Fresh stairs: the item's defaults"), FMath::IsNearlyEqual(Fresh->GetRise(), StairsItem->GetDefaultRise())
			&& Fresh->HasHandrails() == StairsItem->bDefaultHandrails);
		TestEqual(TEXT("...not a repair"), Fresh->GetStateRepairCount(), 0);
	}
	else
	{
		AddError(TEXT("Fresh stairs did not spawn"));
	}

	// A replacement is one step: undo removes the new item and brings the old one back with its id.
	AModularBaseActor* Old = Stage.FindFirst<AStageStairsActor>();
	if (TestNotNull(TEXT("An item to replace"), Old))
	{
		const FStageItemSnapshot OldSnapshot = Old->CaptureSnapshot();
		const int32 Before = Stage.CountPlaced();
		TestTrue(TEXT("Old item removed"), Stage.Spawner->TryDeleteActor(Old));
		AModularBaseActor* New = Stage.Spawner->SpawnItem(DeckItem, OldSnapshot.Transform);
		if (TestNotNull(TEXT("New item placed"), New))
		{
			Stage.EditHistory->RecordReplacement({ OldSnapshot }, { New }, FText::FromString(TEXT("Assistant: replace")));
			TestTrue(TEXT("Undo the replacement"), Stage.EditHistory->Undo() == EStageCommandResult::Succeeded);
			TestEqual(TEXT("...same count"), Stage.CountPlaced(), Before);
			TestNotNull(TEXT("...the old item is back by id"), Stage.Session->FindItemById(OldSnapshot.InstanceId));
			TestTrue(TEXT("Redo the replacement"), Stage.EditHistory->Redo() == EStageCommandResult::Succeeded);
			TestNull(TEXT("...the old item is gone again"), Stage.Session->FindItemById(OldSnapshot.InstanceId));
			TestEqual(TEXT("...same count"), Stage.CountPlaced(), Before);
		}
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
