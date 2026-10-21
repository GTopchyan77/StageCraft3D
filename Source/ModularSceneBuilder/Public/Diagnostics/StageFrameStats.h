// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/** Distribution of one per-frame measurement (milliseconds). Every field is 0 for no samples. */
struct FStageFrameStatsSummary
{
	int32 Count = 0;
	double Mean = 0.0;
	double Median = 0.0;
	double P95 = 0.0;
	double P99 = 0.0;
	double Max = 0.0;
};

/**
 * Pure statistics for the fixture benchmark (Docs/ADR/0012-production-reports-and-fixture-benchmark.md §3). Any thread;
 * covered by StageCraft.Bench.FrameStats.
 */
namespace StageFrameStats
{
	/**
	 * Mean, nearest-rank percentiles (the smallest sample with at least p % of samples at or below it) and maximum. Non-finite
	 * samples are ignored, so one broken reading cannot poison a run.
	 */
	MODULARSCENEBUILDER_API FStageFrameStatsSummary Summarize(TConstArrayView<double> Samples);

	/** Frames that count as dropped: longer than both Budget and Factor x Baseline (a hitch relative to the idle frame). */
	MODULARSCENEBUILDER_API int32 CountDroppedFrames(TConstArrayView<double> FrameMs, double BudgetMs, double BaselineMs, double Factor);
}
