// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"

/**
 * StageCraft's loading screen (Docs/ADR/0024-startup-atmospheres-starter-stages.md §1): the wordmark, a thin progress bar
 * and one status line on the app's window colour.
 *
 * One widget for both startup stages, so the hand-off from the engine's loading screen (MoviePlayer, while the map loads)
 * to the in-app overlay (while the catalogs stream in) looks continuous.
 *
 * - Progress unset: the bar sweeps (indeterminate), for loads that report no fraction (the blocking map load).
 * - Progress set: the bar fills to it and eases towards new values, so coarse jumps still read as motion.
 * - The animation is computed in OnPaint from the paint time; nothing ticks, and it keeps moving on the MoviePlayer's
 *   loading thread, which paints but runs no active timers.
 *
 * It loads no asset, so it can paint before any content has loaded. Colours come from the theme's class defaults,
 * copied at construction on the game thread; the loading thread only reads those copies.
 */
class SStageBrandedLoader : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SStageBrandedLoader)
		: _Progress()
		, _Status()
		, _ShowContinue(false)
	{}
		/** Fraction in [0, 1], or unset for an indeterminate sweep. */
		SLATE_ATTRIBUTE(TOptional<float>, Progress)
		SLATE_ATTRIBUTE(FText, Status)
		/** Shows "Continue" under the status, for a load that failed or is taking too long. */
		SLATE_ATTRIBUTE(bool, ShowContinue)
		SLATE_EVENT(FSimpleDelegate, OnContinue)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/** Fades the whole loader out over Seconds, then calls OnFaded once (game thread, active timer). */
	void FadeOut(float Seconds, FSimpleDelegate OnFaded);

private:
	EActiveTimerReturnType TickFade(double CurrentTime, float DeltaTime);

	FLinearColor Background;
	FLinearColor Accent;
	FLinearColor Track;
	FLinearColor TextPrimary;
	FLinearColor TextSecondary;

	double FadeStart = 0.0;
	float FadeSeconds = 0.f;
	FSimpleDelegate FadeFinished;
};
