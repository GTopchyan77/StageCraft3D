// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Settings/StageGraphicsSettings.h"
#include "StageGraphicsSubsystem.generated.h"

DECLARE_MULTICAST_DELEGATE(FOnStageGraphicsChanged);

/**
 * The single write path for this PC's graphics preferences: quality, render resolution, frame cap and VSync (STATE.md #111).
 *
 * The engine's UGameUserSettings stores them in GameUserSettings.ini and applies the saved values itself at startup
 * (UGameEngine::Start). This subsystem only translates the Preferences panel's choices to it, applies them at once and saves.
 *
 * Why these four: at 500 fixtures the quality level is a GPU lever (Epic -> Low: GPU 9.7 -> 2.4 ms on the development PC),
 * which decides the frame on a slower graphics card; on a fast card the frame is CPU (render-thread) bound and barely moves.
 * The frame cap and VSync keep a laptop cool and quiet when the full rate is not needed.
 *
 * Game thread only. Without GEngine's settings object (commandlets) reads return the defaults and writes do nothing.
 */
UCLASS()
class MODULARSCENEBUILDER_API UStageGraphicsSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	/** The stored choices (Custom quality when the saved groups are not one level). */
	FStageGraphicsChoices GetChoices() const;

	/**
	 * Sanitizes (StageGraphics::Sanitize), stores, applies at once and saves GameUserSettings.ini. Broadcasts OnGraphicsChanged
	 * only when a stored value changed. Returns whether one did.
	 */
	bool SetChoices(const FStageGraphicsChoices& Choices);

	/** After every change that SetChoices stored. */
	FOnStageGraphicsChanged OnGraphicsChanged;

	/** The choices stored in Settings. */
	static FStageGraphicsChoices ReadChoices(const class UGameUserSettings& Settings);

	/**
	 * Stores Choices (sanitized) in Settings without applying or saving them. Quality is written first, because the engine's
	 * single quality level also resets the render resolution; Custom leaves the per-group levels as they are.
	 * Returns whether a stored value changed.
	 */
	static bool WriteChoices(class UGameUserSettings& Settings, const FStageGraphicsChoices& Choices);

private:
	static class UGameUserSettings* GetEngineSettings();
};
