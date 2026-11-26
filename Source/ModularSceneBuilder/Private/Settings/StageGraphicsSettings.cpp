// Copyright Epic Games, Inc. All Rights Reserved.

#include "Settings/StageGraphicsSettings.h"

#include "Algo/AllOf.h"

namespace StageGraphics
{
	TConstArrayView<int32> GetResolutionPresets()
	{
		static const int32 Presets[] = { 0, 50, 67, 75, 100 };
		return Presets;
	}

	TConstArrayView<int32> GetFrameCapPresets()
	{
		static const int32 Presets[] = { 0, 30, 60, 120 };
		return Presets;
	}

	FStageGraphicsChoices Sanitize(const FStageGraphicsChoices& Choices)
	{
		FStageGraphicsChoices Result = Choices;
		if (static_cast<uint8>(Result.Quality) > static_cast<uint8>(EStageGraphicsQuality::Custom))
		{
			Result.Quality = EStageGraphicsQuality::Custom;
		}
		Result.ResolutionPercent = Result.ResolutionPercent <= 0 ? 0 : FMath::Clamp(Result.ResolutionPercent, MinResolutionPercent, MaxResolutionPercent);
		Result.FrameCap = Result.FrameCap <= 0 ? 0 : FMath::Clamp(Result.FrameCap, MinFrameCap, MaxFrameCap);
		return Result;
	}

	int32 ToScalabilityLevel(EStageGraphicsQuality Quality)
	{
		return Quality == EStageGraphicsQuality::Custom ? INDEX_NONE : static_cast<int32>(Quality);
	}

	EStageGraphicsQuality FromGroupLevels(TConstArrayView<int32> Levels)
	{
		if (Levels.IsEmpty())
		{
			return EStageGraphicsQuality::Custom;
		}
		const int32 Level = Levels[0];
		const bool bOneLevel = Algo::AllOf(Levels, [Level](int32 Other) { return Other == Level; });
		return bOneLevel && Level >= 0 && Level <= static_cast<int32>(EStageGraphicsQuality::Epic)
			? static_cast<EStageGraphicsQuality>(Level) : EStageGraphicsQuality::Custom;
	}
}
