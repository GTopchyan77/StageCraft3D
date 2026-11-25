// Copyright Epic Games, Inc. All Rights Reserved.

// World test for the shadow budget's moving-caster limit (Docs/ADR/0018 §4, STATE.md #108) through UShowControlSubsystem's timer:
// lit fixtures cast up to the budget, panning them caps the moving casters, and once they stand still the subsystem re-ranks on
// its own (the retry armed from inside its timer callback) and gives their shadows back.
// Run: UnrealEditor-Cmd <project> -ExecCmds="Automation RunTests StageCraft.Show.ShadowBudgetWorld; Quit" -unattended -nullrhi

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Actors/LightingFixtureActor.h"
#include "Data/LightingFixtureData.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/WorldSettings.h"
#include "Misc/AutomationTest.h"
#include "Show/StageShadowBudget.h"
#include "Subsystems/ShowControlSubsystem.h"
#include "Data/StageParameterTypes.h"

namespace StageShadowBudgetWorldTests
{
	/** A game world that has begun play; fixtures register with its show control on BeginPlay. Torn down by Reset. */
	struct FTestState
	{
		UWorld* World = nullptr;
		TArray<TWeakObjectPtr<ALightingFixtureActor>> Fixtures;
		int32 Step = 0;
		double StepStartWorldSeconds = 0.0;
		double Deadline = 0.0;

		void Create(int32 FixtureCount)
		{
			World = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false, TEXT("StageShadowBudgetWorldTestWorld"));
			FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
			Context.SetCurrentWorld(World);
			World->InitializeActorsForPlay(FURL());
			World->BeginPlay();
			World->GetWorldSettings()->NotifyBeginPlay();

			ULightingFixtureData* Data = NewObject<ULightingFixtureData>(GetTransientPackage());
			Data->bHasPanTilt = true;
			for (int32 Index = 0; Index < FixtureCount; ++Index)
			{
				const FTransform Where(FVector(Index * 100.0, 0.0, 600.0));
				ALightingFixtureActor* Fixture = World->SpawnActorDeferred<ALightingFixtureActor>(ALightingFixtureActor::StaticClass(), Where);
				Fixture->InitializeFromItemData(Data);
				Fixture->FinishSpawning(Where);
				Fixtures.Add(Fixture);
			}
		}

		void Destroy()
		{
			if (World)
			{
				World->BeginTearingDown();
				World->EndPlay(EEndPlayReason::Quit);
				World->DestroyWorld(/*bInformEngineOfWorld*/ false);
				GEngine->DestroyWorldContext(World);
				World = nullptr;
			}
		}

		int32 CountCasting() const
		{
			int32 Count = 0;
			for (const TWeakObjectPtr<ALightingFixtureActor>& Fixture : Fixtures)
			{
				Count += Fixture.IsValid() && Fixture->AreBeamShadowsEnabled() ? 1 : 0;
			}
			return Count;
		}

		void SetAll(const FGameplayTag& Attribute, float Value)
		{
			for (const TWeakObjectPtr<ALightingFixtureActor>& Fixture : Fixtures)
			{
				Fixture->SetAttribute(Attribute, Value);
			}
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageShadowBudgetWorldTest, "StageCraft.Show.ShadowBudgetWorld",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FStageShadowBudgetWorldTest::RunTest(const FString& Parameters)
{
	using namespace StageShadowBudgetWorldTests;
	const UStageShowPerformanceSettings* Settings = GetDefault<UStageShowPerformanceSettings>();
	const FStageShadowLimits Limits = Settings->GetShadowLimits();
	constexpr int32 FixtureCount = 20;
	// The test needs the moving limit to bind below both the fixture count and the total (the defaults are 16 of 64).
	if (Limits.Total < 0 || Limits.Moving < 0 || Limits.Moving >= FMath::Min(FixtureCount, Limits.Total))
	{
		AddWarning(FString::Printf(TEXT("Skipped: the project's shadow limits (%d total, %d moving) do not cap %d moving fixtures."), Limits.Total, Limits.Moving, FixtureCount));
		return true;
	}
	const int32 StillCasters = FMath::Min(FixtureCount, Limits.Total);
	const int32 MovingCasters = Limits.Moving;
	// Long enough for the movement to end, the dropped fixtures' hold to run out and one update interval, with a margin.
	const double SettleSeconds = Settings->GetMovingSeconds() + Settings->GetMinHoldSeconds() + 2.0 * Settings->GetUpdateSeconds() + 1.0;

	TSharedRef<FTestState> State = MakeShared<FTestState>();
	State->Create(FixtureCount);
	TestEqual(TEXT("Fixtures registered"), UWorld::GetSubsystem<UShowControlSubsystem>(State->World)->GetFixtures().Num(), FixtureCount);
	// Spawning applies pan and tilt before play: that is not a movement.
	for (const TWeakObjectPtr<ALightingFixtureActor>& Fixture : State->Fixtures)
	{
		TestFalse(TEXT("No movement before play"), Fixture->GetLastHeadMoveTime().IsSet());
	}
	// Lighting them up marks the ranking dirty; the subsystem's timer ranks.
	State->SetAll(StageCraftTags::Attribute_Dimmer, 1.f);
	State->Deadline = FPlatformTime::Seconds() + 30.0;

	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, State, StillCasters, MovingCasters, SettleSeconds]() -> bool
	{
		// A tenth of a second of world time per engine frame (world timers tick once per frame).
		State->World->Tick(LEVELTICK_All, 0.1f);
		const double WorldSeconds = State->World->GetTimeSeconds();
		const int32 Casting = State->CountCasting();
		const bool bTimedOut = FPlatformTime::Seconds() > State->Deadline;

		switch (State->Step)
		{
		case 0: // Still and lit: all within the budget cast. Then the heads pan once.
			if (Casting != StillCasters && !bTimedOut)
			{
				return false;
			}
			TestEqual(TEXT("Still: every lit fixture within the budget casts"), Casting, StillCasters);
			State->SetAll(StageCraftTags::Attribute_Pan, 45.f);
			State->StepStartWorldSeconds = WorldSeconds;
			State->Step = 1;
			return false;

		case 1: // Moving: only the moving limit casts, within one update interval.
			if (Casting != MovingCasters && !bTimedOut)
			{
				return false;
			}
			TestEqual(TEXT("Moving: casters capped at the moving limit"), Casting, MovingCasters);
			TestTrue(TEXT("... within a second of the pan"), WorldSeconds - State->StepStartWorldSeconds <= 1.0);
			State->StepStartWorldSeconds = WorldSeconds;
			State->Step = 2;
			return false;

		case 2: // Stopped: with no other change, the subsystem's own retry re-ranks and the shadows come back.
			if (Casting != StillCasters && !bTimedOut && WorldSeconds - State->StepStartWorldSeconds <= SettleSeconds)
			{
				return false;
			}
			TestEqual(TEXT("Stopped: shadows back for every lit fixture within the budget"), Casting, StillCasters);
			TestTrue(*FString::Printf(TEXT("... with no call from outside, within %.1f s of world time"), SettleSeconds),
				WorldSeconds - State->StepStartWorldSeconds <= SettleSeconds);
			State->Destroy();
			return true;

		default:
			State->Destroy();
			return true;
		}
	}));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
