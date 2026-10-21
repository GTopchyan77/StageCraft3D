// Copyright Epic Games, Inc. All Rights Reserved.

#include "Show/ShowControlTypes.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(ShowControlTypes)

namespace StageCueRules
{
	/**
	 * The cue number in whole thousandths. Done in double: near 10000 a float step is about 0.001 itself, so float arithmetic
	 * would let 10000 pass as 9999.999 and turn 1.5 into 1.5000001.
	 */
	int64 ToThousandths(float CueNumber)
	{
		return FMath::RoundToInt64(static_cast<double>(CueNumber) * 1000.0);
	}

	float RoundCueNumber(float CueNumber)
	{
		// Divided, not multiplied by 0.001, so the result is the float nearest the decimal number (1.5 stays exactly 1.5).
		return static_cast<float>(static_cast<double>(ToThousandths(CueNumber)) / 1000.0);
	}

	bool IsValidCueNumber(float CueNumber)
	{
		if (!FMath::IsFinite(CueNumber) || FMath::Abs(CueNumber) > 1.e7f)
		{
			return false;
		}
		const int64 Thousandths = ToThousandths(CueNumber);
		return Thousandths >= ToThousandths(MinCueNumber) && Thousandths <= ToThousandths(MaxCueNumber);
	}

	float SanitizeFadeTime(float FadeTime)
	{
		return FMath::IsFinite(FadeTime) ? FMath::Clamp(FadeTime, 0.f, MaxFadeTime) : 0.f;
	}

	FText SanitizeLabel(const FText& Label)
	{
		const FString Trimmed = Label.ToString().TrimStartAndEnd();
		if (Trimmed.Len() <= MaxLabelLength)
		{
			// Keeps the original FText (and its localization) when nothing had to change.
			return Trimmed.Len() == Label.ToString().Len() ? Label : FText::FromString(Trimmed);
		}
		return FText::FromString(Trimmed.Left(MaxLabelLength));
	}

	int32 SanitizeCueList(TArray<FShowCue>& Cues)
	{
		const int32 Before = Cues.Num();
		TArray<FShowCue> Kept;
		Kept.Reserve(Cues.Num());
		for (FShowCue& Cue : Cues)
		{
			if (!IsValidCueNumber(Cue.Number))
			{
				continue;
			}
			Cue.Number = RoundCueNumber(Cue.Number);
			const bool bDuplicate = Kept.ContainsByPredicate([&Cue](const FShowCue& Other)
			{
				return FMath::IsNearlyEqual(Other.Number, Cue.Number, NumberResolution * 0.5f);
			});
			if (bDuplicate)
			{
				continue;
			}
			Cue.FadeTime = SanitizeFadeTime(Cue.FadeTime);
			Cue.Label = SanitizeLabel(Cue.Label);
			Kept.Add(MoveTemp(Cue));
		}
		// Stable, so the order of equal-looking input never decides which duplicate survived (that was decided above).
		Kept.StableSort([](const FShowCue& A, const FShowCue& B) { return A.Number < B.Number; });
		Cues = MoveTemp(Kept);
		return Before - Cues.Num();
	}
}
