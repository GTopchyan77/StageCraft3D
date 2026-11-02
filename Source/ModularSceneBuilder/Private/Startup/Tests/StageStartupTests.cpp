// Copyright Epic Games, Inc. All Rights Reserved.

// Startup progress (Docs/ADR/0024-startup-atmospheres-starter-stages.md §2).
//  - Readiness: the pure rules (weighted progress, combined state, the status source) and the subsystem's contract on a live
//    game instance: the real catalogs settle it as Ready at 100 %, the state only moves forward, a late source is refused,
//    and reports for unknown or settled sources change nothing.
// Run: UnrealEditor-Cmd <project> -ExecCmds="Automation RunTests StageCraft.Startup.Readiness; Quit" -unattended

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Startup/StageStartupSubsystem.h"
#include "Tests/StageTestFixtures.h"

namespace StageStartupTests
{
	FStageStartupSource MakeSource(FName Id, float Weight, float Progress, EStageStartupSourceState State)
	{
		FStageStartupSource Source;
		Source.Id = Id;
		Source.Label = FText::FromName(Id);
		Source.Weight = Weight;
		Source.Progress = Progress;
		Source.State = State;
		return Source;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageStartupReadinessTest, "StageCraft.Startup.Readiness", StageTestFixtures::Flags)

bool FStageStartupReadinessTest::RunTest(const FString& Parameters)
{
	using namespace StageStartupTests;
	using namespace StageTestFixtures;

	// --- Pure rules ---
	{
		TArray<FStageStartupSource> None;
		TestEqual(TEXT("No sources: complete"), StageStartup::CombinedProgress(None), 1.f);
		TestTrue(TEXT("No sources: ready"), StageStartup::CombinedState(None, false) == EStageStartupState::Ready);

		TArray<FStageStartupSource> Sources = {
			MakeSource(TEXT("Big"), 3.f, 0.5f, EStageStartupSourceState::Loading),
			MakeSource(TEXT("Small"), 1.f, 0.f, EStageStartupSourceState::Loading) };
		TestEqual(TEXT("Weighted: (3 x 0.5 + 1 x 0) / 4"), StageStartup::CombinedProgress(Sources), 0.375f);
		TestTrue(TEXT("Loading while anything loads"), StageStartup::CombinedState(Sources, false) == EStageStartupState::Loading);
		TestTrue(TEXT("Timed out while anything loads after the timeout"), StageStartup::CombinedState(Sources, true) == EStageStartupState::TimedOut);
		TestEqual(TEXT("The status names the first source still loading"), StageStartup::FirstLoading(Sources)->Id, FName(TEXT("Big")));

		Sources[0].State = EStageStartupSourceState::Failed;
		Sources[0].Progress = 0.2f;
		TestEqual(TEXT("A failed source counts as settled: (3 + 0) / 4"), StageStartup::CombinedProgress(Sources), 0.75f);
		TestEqual(TEXT("A failed source is not named as loading"), StageStartup::FirstLoading(Sources)->Id, FName(TEXT("Small")));

		Sources[1].State = EStageStartupSourceState::Ready;
		TestTrue(TEXT("Failed once everything settled and one failed"), StageStartup::CombinedState(Sources, true) == EStageStartupState::Failed);
		TestNull(TEXT("Nothing loading"), StageStartup::FirstLoading(Sources));

		Sources[0].State = EStageStartupSourceState::Ready;
		TestTrue(TEXT("Ready once everything is ready, timeout or not"), StageStartup::CombinedState(Sources, true) == EStageStartupState::Ready);

		TArray<FStageStartupSource> Odd = {
			MakeSource(TEXT("Negative"), -5.f, 0.5f, EStageStartupSourceState::Loading),
			MakeSource(TEXT("Over"), 1.f, 7.f, EStageStartupSourceState::Loading) };
		TestEqual(TEXT("Negative weights count as 0 and progress is clamped"), StageStartup::CombinedProgress(Odd), 1.f);
	}

	// --- The subsystem on a live game instance ---
	FStageTestGameInstance Game;
	if (!TestTrue(TEXT("The test game instance starts"), Game.IsValid()))
	{
		return false;
	}
	UStageStartupSubsystem* Startup = Game.Subsystem<UStageStartupSubsystem>();
	if (!TestNotNull(TEXT("Startup subsystem"), Startup))
	{
		return false;
	}

	int32 SettledCount = 0;
	Startup->OnSettled.AddLambda([&SettledCount](EStageStartupState) { ++SettledCount; });
	const bool bSettled = Game.Pump([Startup]() { return Startup->IsSettled(); }, 20.0);
	TestTrue(TEXT("Startup settles once the catalogs have loaded"), bSettled);
	TestTrue(TEXT("Ready"), Startup->GetState() == EStageStartupState::Ready);
	TestEqual(TEXT("At 100 %"), Startup->GetProgress(), 1.f);
	TestTrue(TEXT("Waits for the library and the shop"),
		Startup->GetSources().ContainsByPredicate([](const FStageStartupSource& Source) { return Source.Id == TEXT("Library"); })
		&& Startup->GetSources().ContainsByPredicate([](const FStageStartupSource& Source) { return Source.Id == TEXT("Shop"); }));
	TestTrue(TEXT("OnSettled fires at most once (zero when it settled during Initialize)"), SettledCount <= 1);

	const int32 SourceCount = Startup->GetSources().Num();
	TestFalse(TEXT("A source registered after settling is refused"), Startup->RegisterSource(TEXT("Late"), FText::GetEmpty(), 1.f));
	TestEqual(TEXT("…and not added"), Startup->GetSources().Num(), SourceCount);
	Startup->ReportFailed(TEXT("Library"), FText::FromString(TEXT("test")));
	Startup->ReportProgress(TEXT("Unknown"), 0.5f);
	TestTrue(TEXT("Reports after settling change nothing"), Startup->GetState() == EStageStartupState::Ready);
	TestEqual(TEXT("Status"), Startup->GetStatusText().ToString(), FString(TEXT("Ready")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
