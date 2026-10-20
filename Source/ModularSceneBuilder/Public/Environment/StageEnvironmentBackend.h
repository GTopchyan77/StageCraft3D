// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "StageEnvironmentBackend.generated.h"

/**
 * Boundary between StageCraft's environment settings and a sky / weather system (Docs/ADR/0009-outdoor-environment.md).
 *
 * StageCraft only deals in FStageEnvironmentSettings; a backend turns them into a sky. The production backend wraps the
 * Sky Creator plugin in its own module (StageCraftSkyCreator), so this module never depends on a marketplace plugin;
 * UStageEnvironmentSubsystem creates the backend named in its config.
 *
 * Contract:
 *  - Called on the game thread only, by the owning UStageEnvironmentSubsystem, after the world has begun play.
 *  - Apply receives sanitized settings, and only when they changed. bSkyEnabled false means: remove everything the backend
 *    added and restore everything it changed, so the indoor stage is exactly as the level made it.
 *  - Shutdown is called once when the world ends; it must tolerate a world that is already tearing down, and a second call.
 *  - A backend reports failures by logging; it never throws away the subsystem's settings.
 */
UCLASS(Abstract)
class MODULARSCENEBUILDER_API UStageEnvironmentBackend : public UObject
{
	GENERATED_BODY()

public:
	virtual void Apply(UWorld& World, const struct FStageEnvironmentSettings& Settings) PURE_VIRTUAL(UStageEnvironmentBackend::Apply, );

	virtual void Shutdown() {}

	/** User-facing name of the sky system, shown in the Environment panel. */
	virtual FText GetDisplayName() const PURE_VIRTUAL(UStageEnvironmentBackend::GetDisplayName, return FText::GetEmpty(););
};
