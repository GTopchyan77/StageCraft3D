// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/WeakObjectPtrTemplates.h"

/**
 * The level's own sun, sky light, atmosphere, clouds and fog, hidden while something else lights the stage (the outdoor sky
 * backend or the studio rig, Docs/ADR/0024 §4) and shown again afterwards, so the stage is never lit twice.
 *
 * One pass over the world when hiding, not a poll. Only components that were visible are hidden, and only those are shown
 * again. Weak references: the level may unload first. Game thread only.
 */
class MODULARSCENEBUILDER_API FStageHiddenLevelLighting
{
public:
	/** Hides every visible level lighting component except those of Owner (the actor that replaces them). Idempotent. */
	void Hide(class UWorld& World, const class AActor& Owner);

	/** Shows what Hide hid. Safe to call twice and after the level has unloaded. */
	void Restore();

	/** Forgets what was hidden without touching it: for a world that is tearing down and destroys it all anyway. */
	void Forget() { Hidden.Reset(); }

	int32 Num() const { return Hidden.Num(); }

private:
	TArray<TWeakObjectPtr<class USceneComponent>> Hidden;
};
