// Copyright Epic Games, Inc. All Rights Reserved.

// Tests for the show control's fixture registry (STATE.md #104): lookup by fixture ID follows registration, renumbering and removal,
// duplicate IDs are renumbered on arrival, and registering a large rig costs about the same per fixture as a small one (the lookup
// used to scan every registered fixture, so loading a scene was quadratic in its fixture count).
// Run: UnrealEditor-Cmd <project> -ExecCmds="Automation RunTests StageCraft.Show.FixtureRegistry; Quit" -unattended -nullrhi

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Actors/LightingFixtureActor.h"
#include "Data/LightingFixtureData.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "Subsystems/ShowControlSubsystem.h"

namespace StageFixtureRegistryTests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter;

	/** A game world that has begun play, destroyed with the scope. Fixtures register with its show control on BeginPlay. */
	struct FScopedWorld
	{
		UWorld* World = nullptr;
		ULightingFixtureData* Data = nullptr;

		FScopedWorld()
		{
			World = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false, TEXT("StageFixtureRegistryTestWorld"));
			FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
			Context.SetCurrentWorld(World);
			World->InitializeActorsForPlay(FURL());
			World->BeginPlay();
			World->GetWorldSettings()->NotifyBeginPlay();
			Data = NewObject<ULightingFixtureData>(GetTransientPackage());
		}

		~FScopedWorld()
		{
			World->BeginTearingDown();
			World->EndPlay(EEndPlayReason::Quit);
			World->DestroyWorld(/*bInformEngineOfWorld*/ false);
			GEngine->DestroyWorldContext(World);
		}

		/** A fixture that asks for FixtureId (0: any), as a loaded scene's fixtures do before they begin play. */
		ALightingFixtureActor* Spawn(int32 FixtureId = 0) const
		{
			ALightingFixtureActor* Fixture = World->SpawnActorDeferred<ALightingFixtureActor>(ALightingFixtureActor::StaticClass(), FTransform::Identity);
			Fixture->InitializeFromItemData(Data);
			if (FixtureId > 0)
			{
				Fixture->PrepareImportedPatch(FixtureId, FStageDMXPatch(), TOptional<FVector2D>());
			}
			Fixture->FinishSpawning(FTransform::Identity);
			return Fixture;
		}

		UShowControlSubsystem* Show() const { return World->GetSubsystem<UShowControlSubsystem>(); }
	};

	/**
	 * Seconds to register the first Count fixtures of Rig (keeping their IDs) into a registry holding none of Rig: the show control's
	 * part of loading a scene of Count fixtures. The rest of Rig is registered again afterwards, untimed.
	 */
	double TimeRegistration(UShowControlSubsystem& Show, TConstArrayView<ALightingFixtureActor*> Rig, int32 Count)
	{
		for (ALightingFixtureActor* Fixture : Rig)
		{
			Show.UnregisterFixture(Fixture);
		}
		const double Start = FPlatformTime::Seconds();
		for (int32 Index = 0; Index < Count; ++Index)
		{
			Show.RegisterFixture(Rig[Index]);
		}
		const double Seconds = FPlatformTime::Seconds() - Start;
		for (int32 Index = Count; Index < Rig.Num(); ++Index)
		{
			Show.RegisterFixture(Rig[Index]);
		}
		return Seconds;
	}

	/**
	 * Seconds per FindFixture with only the first Count fixtures of Rig registered (Rig's IDs start at FirstId), or -1 when one was
	 * not found. Rig is whole again afterwards.
	 */
	double TimeLookup(UShowControlSubsystem& Show, TConstArrayView<ALightingFixtureActor*> Rig, int32 Count, int32 FirstId)
	{
		for (int32 Index = Count; Index < Rig.Num(); ++Index)
		{
			Show.UnregisterFixture(Rig[Index]);
		}
		constexpr int32 Lookups = 20000;
		int32 Found = 0;
		const double Start = FPlatformTime::Seconds();
		for (int32 Index = 0; Index < Lookups; ++Index)
		{
			Found += Show.FindFixture(FirstId + Index % Count) ? 1 : 0;
		}
		// Every ID asked for is registered; a miss would time a different path, so it reports -1 (the caller's test fails).
		const double Seconds = Found == Lookups ? (FPlatformTime::Seconds() - Start) / Lookups : -1.0;
		for (int32 Index = Count; Index < Rig.Num(); ++Index)
		{
			Show.RegisterFixture(Rig[Index]);
		}
		return Seconds;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageShowFixtureRegistryTest, "StageCraft.Show.FixtureRegistry", StageFixtureRegistryTests::Flags)

bool FStageShowFixtureRegistryTest::RunTest(const FString& Parameters)
{
	using namespace StageFixtureRegistryTests;
	FScopedWorld Scope;
	UShowControlSubsystem* Show = Scope.Show();
	if (!TestNotNull(TEXT("Show control"), Show))
	{
		return false;
	}

	// New fixtures take the next free IDs and are found by them.
	ALightingFixtureActor* A = Scope.Spawn();
	ALightingFixtureActor* B = Scope.Spawn();
	ALightingFixtureActor* C = Scope.Spawn();
	TestEqual(TEXT("IDs assigned in order"), FString::Printf(TEXT("%d %d %d"), A->GetFixtureId(), B->GetFixtureId(), C->GetFixtureId()), FString(TEXT("1 2 3")));
	TestTrue(TEXT("Found by ID"), Show->FindFixture(1) == A && Show->FindFixture(2) == B && Show->FindFixture(3) == C);
	TestNull(TEXT("Unused ID finds nothing"), Show->FindFixture(4));
	TestNull(TEXT("ID 0 finds nothing"), Show->FindFixture(0));
	TestEqual(TEXT("Next free ID"), Show->GetNextFreeFixtureId(), 4);

	// Registering twice changes nothing.
	Show->RegisterFixture(B);
	TestEqual(TEXT("Registered once"), Show->GetFixtures().Num(), 3);
	TestTrue(TEXT("Still found after a second registration"), Show->FindFixture(2) == B);

	// Renumbering moves the fixture's entry: the old ID is free, the new one finds it.
	TestTrue(TEXT("Renumber 2 -> 10"), B->SetFixtureId(10));
	TestTrue(TEXT("Found by its new ID"), Show->FindFixture(10) == B);
	TestNull(TEXT("Old ID finds nothing"), Show->FindFixture(2));
	TestTrue(TEXT("Old ID is free"), Show->IsFixtureIdFree(2));
	TestFalse(TEXT("New ID is taken"), Show->IsFixtureIdFree(10));
	TestTrue(TEXT("New ID is free for its own fixture"), Show->IsFixtureIdFree(10, B));
	TestEqual(TEXT("Next free ID follows the highest"), Show->GetNextFreeFixtureId(), 11);

	// A taken ID is refused and nothing moves.
	AddExpectedMessage(TEXT("fixture ID 1 is already in use"), EAutomationExpectedMessageFlags::Contains, 1);
	TestFalse(TEXT("Renumber to a taken ID refused"), C->SetFixtureId(1));
	TestTrue(TEXT("Refusal leaves both fixtures where they were"), Show->FindFixture(1) == A && Show->FindFixture(3) == C);

	// A fixture that arrives with a taken ID (a merged scene) is renumbered; the fixture that had the ID keeps it.
	ALightingFixtureActor* Duplicate = Scope.Spawn(1);
	TestEqual(TEXT("Duplicate renumbered to the next free ID"), Duplicate->GetFixtureId(), 11);
	TestTrue(TEXT("Original keeps its ID"), Show->FindFixture(1) == A);
	TestTrue(TEXT("Duplicate found by its new ID"), Show->FindFixture(11) == Duplicate);

	// A fixture that arrives with a free ID keeps it, even below the highest.
	ALightingFixtureActor* Loaded = Scope.Spawn(2);
	TestEqual(TEXT("Loaded fixture keeps its free ID"), Loaded->GetFixtureId(), 2);
	TestTrue(TEXT("Found by it"), Show->FindFixture(2) == Loaded);

	// Removal frees the ID; the highest ID going away lowers the next free one.
	Duplicate->Destroy();
	TestNull(TEXT("Removed fixture not found"), Show->FindFixture(11));
	TestTrue(TEXT("Its ID is free again"), Show->IsFixtureIdFree(11));
	TestEqual(TEXT("Next free ID after removing the highest"), Show->GetNextFreeFixtureId(), 11);
	B->Destroy();
	TestNull(TEXT("Renumbered fixture removed under its new ID"), Show->FindFixture(10));
	TestEqual(TEXT("Next free ID after removing 10"), Show->GetNextFreeFixtureId(), 4);
	TestEqual(TEXT("Three fixtures left"), Show->GetFixtures().Num(), 3);

	// An unregistered fixture renumbered while out of the registry is found by its new ID once it is back.
	Show->UnregisterFixture(C);
	TestNull(TEXT("Unregistered fixture not found"), Show->FindFixture(3));
	TestTrue(TEXT("Renumber while unregistered"), C->SetFixtureId(7));
	TestNull(TEXT("Not found before it registers again"), Show->FindFixture(7));
	Show->RegisterFixture(C);
	TestTrue(TEXT("Found by its new ID after registering again"), Show->FindFixture(7) == C);

	// GetFixtures stays sorted by ID whatever the registration order.
	const TArray<ALightingFixtureActor*> Sorted = Show->GetFixtures();
	TestTrue(TEXT("GetFixtures sorted by ID"), Sorted.Num() == 3 && Sorted[0] == A && Sorted[1] == Loaded && Sorted[2] == C);

	// Cost against rig size. Registration is only logged: its fixed per-fixture cost (the log line, the shadow budget) hides the
	// lookup's share. The guard is the lookup alone, small rig against large rig with a wide margin: a scan of the rig took about 8x
	// as long per lookup at 8x the fixtures (STATE.md #104 baseline), a keyed lookup about the same.
	TArray<ALightingFixtureActor*> Rig;
	constexpr int32 SmallRig = 64;
	constexpr int32 LargeRig = 512;
	Rig.Reserve(LargeRig);
	for (int32 Index = 0; Index < LargeRig; ++Index)
	{
		Rig.Add(Scope.Spawn(100 + Index));
	}
	TestEqual(TEXT("Large rig registered"), Show->GetFixtures().Num(), 3 + LargeRig);
	TestTrue(TEXT("Large rig found by ID"), Show->FindFixture(100) == Rig[0] && Show->FindFixture(100 + LargeRig - 1) == Rig.Last());

	// Best of three, so one descheduled run does not decide it.
	double SmallSeconds = TNumericLimits<double>::Max();
	double LargeSeconds = TNumericLimits<double>::Max();
	for (int32 Run = 0; Run < 3; ++Run)
	{
		SmallSeconds = FMath::Min(SmallSeconds, TimeRegistration(*Show, Rig, SmallRig));
		LargeSeconds = FMath::Min(LargeSeconds, TimeRegistration(*Show, Rig, LargeRig));
	}
	const double SmallPerFixture = SmallSeconds / SmallRig;
	const double LargePerFixture = LargeSeconds / LargeRig;

	double SmallLookup = TNumericLimits<double>::Max();
	double LargeLookup = TNumericLimits<double>::Max();
	for (int32 Run = 0; Run < 3; ++Run)
	{
		SmallLookup = FMath::Min(SmallLookup, TimeLookup(*Show, Rig, SmallRig, 100));
		LargeLookup = FMath::Min(LargeLookup, TimeLookup(*Show, Rig, LargeRig, 100));
	}
	AddInfo(FString::Printf(TEXT("Registration: %d fixtures %.3f ms (%.2f us each), %d fixtures %.3f ms (%.2f us each). Lookup: %.3f us at %d, %.3f us at %d."),
		SmallRig, SmallSeconds * 1000.0, SmallPerFixture * 1e6, LargeRig, LargeSeconds * 1000.0, LargePerFixture * 1e6,
		SmallLookup * 1e6, SmallRig, LargeLookup * 1e6, LargeRig));
	TestTrue(TEXT("Every looked-up fixture found"), SmallLookup >= 0.0 && LargeLookup >= 0.0);
	TestTrue(TEXT("Lookup cost does not grow with the rig (under 3x at 8x the fixtures)"), LargeLookup < SmallLookup * 3.0);

	TestEqual(TEXT("Every fixture registered again"), Show->GetFixtures().Num(), 3 + LargeRig);
	TestTrue(TEXT("Earlier fixtures untouched by the rig"), Show->FindFixture(1) == A && Show->FindFixture(2) == Loaded && Show->FindFixture(7) == C);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
