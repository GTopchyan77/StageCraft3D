// Copyright Epic Games, Inc. All Rights Reserved.

#include "Diagnostics/StageFrameStats.h"

namespace StageFrameStats
{
	namespace
	{
		/** Nearest-rank percentile of ascending samples (not empty). */
		double GetPercentile(const TArray<double>& Sorted, double Percent)
		{
			const int32 Rank = FMath::CeilToInt32(Percent / 100.0 * Sorted.Num());
			return Sorted[FMath::Clamp(Rank - 1, 0, Sorted.Num() - 1)];
		}
	}

	FStageFrameStatsSummary Summarize(TConstArrayView<double> Samples)
	{
		TArray<double> Sorted;
		Sorted.Reserve(Samples.Num());
		for (const double Sample : Samples)
		{
			if (FMath::IsFinite(Sample))
			{
				Sorted.Add(Sample);
			}
		}

		FStageFrameStatsSummary Summary;
		if (Sorted.IsEmpty())
		{
			return Summary;
		}

		Sorted.Sort();
		double Sum = 0.0;
		for (const double Sample : Sorted)
		{
			Sum += Sample;
		}
		Summary.Count = Sorted.Num();
		Summary.Mean = Sum / Sorted.Num();
		Summary.Median = GetPercentile(Sorted, 50.0);
		Summary.P95 = GetPercentile(Sorted, 95.0);
		Summary.P99 = GetPercentile(Sorted, 99.0);
		Summary.Max = Sorted.Last();
		return Summary;
	}

	int32 CountDroppedFrames(TConstArrayView<double> FrameMs, double BudgetMs, double BaselineMs, double Factor)
	{
		const double Threshold = FMath::Max(BudgetMs, BaselineMs * Factor);
		int32 Dropped = 0;
		for (const double Frame : FrameMs)
		{
			Dropped += FMath::IsFinite(Frame) && Frame > Threshold ? 1 : 0;
		}
		return Dropped;
	}
}
