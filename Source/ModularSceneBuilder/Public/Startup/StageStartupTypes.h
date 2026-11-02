// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/** Load progress of a catalog, in [0, 1]. Broadcast while its assets stream in (Docs/ADR/0024 §2). */
DECLARE_MULTICAST_DELEGATE_OneParam(FOnStageLoadProgress, float /*Fraction*/);

/** Where one thing the app needs at startup stands. */
enum class EStageStartupSourceState : uint8
{
	Loading,
	Ready,
	Failed
};

/** Where the whole startup stands (UStageStartupSubsystem). */
enum class EStageStartupState : uint8
{
	/** Something is still loading and the timeout has not passed. */
	Loading,
	/** Every source is ready: the app is fully usable. */
	Ready,
	/** Every source has settled and at least one failed. The app is usable; what failed refuses its own requests. */
	Failed,
	/** The timeout passed with something still loading. The app is usable; what is still loading says so where it is used. */
	TimedOut
};

/** One thing the app waits for at startup: a catalog, the panel definitions, the atmosphere presets. */
struct FStageStartupSource
{
	FName Id;
	/** What the user is told is loading ("the library"). */
	FText Label;
	/** Share of the progress bar, relative to the other sources. Larger catalogs get more. */
	float Weight = 1.f;
	float Progress = 0.f;
	EStageStartupSourceState State = EStageStartupSourceState::Loading;
	/** Why it failed, for the loader; empty unless Failed. */
	FText FailureReason;
};

/** Pure rules of the startup progress, kept apart so they can be tested without a game instance. */
namespace StageStartup
{
	/** Weighted progress in [0, 1]. A settled source (ready or failed) counts as complete. No sources: 1. */
	MODULARSCENEBUILDER_API float CombinedProgress(TConstArrayView<FStageStartupSource> Sources);

	/** The overall state from the sources, given whether the timeout has passed. */
	MODULARSCENEBUILDER_API EStageStartupState CombinedState(TConstArrayView<FStageStartupSource> Sources, bool bTimedOut);

	/** The first source still loading (the one the status line names), or null. */
	MODULARSCENEBUILDER_API const FStageStartupSource* FirstLoading(TConstArrayView<FStageStartupSource> Sources);
}
