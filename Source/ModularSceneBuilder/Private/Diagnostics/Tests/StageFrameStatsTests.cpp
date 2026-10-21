// Copyright Epic Games, Inc. All Rights Reserved.

// Unit tests for the fixture benchmark's statistics (Docs/ADR/0012-production-reports-and-fixture-benchmark.md §3). No world.
// Run: UnrealEditor-Cmd <project> -ExecCmds="Automation RunTests StageCraft.Bench; Quit" -unattended -nullrhi

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Diagnostics/StageFrameStats.h"
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageFrameStatsTest, "StageCraft.Bench.FrameStats",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FStageFrameStatsTest::RunTest(const FString& Parameters)
{
	const FStageFrameStatsSummary Empty = StageFrameStats::Summarize({});
	TestEqual(TEXT("No samples: count 0"), Empty.Count, 0);
	TestEqual(TEXT("No samples: max 0"), Empty.Max, 0.0);

	// 1..100 in reverse order: nearest-rank percentiles are the sample values themselves.
	TArray<double> Hundred;
	for (int32 Value = 100; Value >= 1; --Value)
	{
		Hundred.Add(Value);
	}
	const FStageFrameStatsSummary Stats = StageFrameStats::Summarize(Hundred);
	TestEqual(TEXT("Count"), Stats.Count, 100);
	TestEqual(TEXT("Mean"), Stats.Mean, 50.5, 1e-9);
	TestEqual(TEXT("Median"), Stats.Median, 50.0);
	TestEqual(TEXT("P95"), Stats.P95, 95.0);
	TestEqual(TEXT("P99"), Stats.P99, 99.0);
	TestEqual(TEXT("Max"), Stats.Max, 100.0);

	const FStageFrameStatsSummary One = StageFrameStats::Summarize({ 16.7 });
	TestTrue(TEXT("One sample is every percentile"), One.Median == 16.7 && One.P99 == 16.7 && One.Max == 16.7);

	const double NaN = std::numeric_limits<double>::quiet_NaN();
	const FStageFrameStatsSummary Broken = StageFrameStats::Summarize({ 10.0, NaN, 20.0, std::numeric_limits<double>::infinity() });
	TestEqual(TEXT("Non-finite samples are ignored"), Broken.Count, 2);
	TestEqual(TEXT("Mean of the finite samples"), Broken.Mean, 15.0, 1e-9);

	// Dropped: over max(budget, factor x baseline).
	const TArray<double> Frames = { 8.0, 16.0, 17.0, 20.0, 40.0, NaN };
	TestEqual(TEXT("Budget dominates a fast baseline"), StageFrameStats::CountDroppedFrames(Frames, 16.7, 8.0, 1.5), 3);
	TestEqual(TEXT("A slow baseline raises the threshold"), StageFrameStats::CountDroppedFrames(Frames, 16.7, 20.0, 1.5), 1);
	TestEqual(TEXT("Nothing to count"), StageFrameStats::CountDroppedFrames({}, 16.7, 8.0, 1.5), 0);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
