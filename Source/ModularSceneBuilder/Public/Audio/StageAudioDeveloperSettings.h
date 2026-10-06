// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "GameplayTagContainer.h"
#include "StageAudioDeveloperSettings.generated.h"

/** One feedback cue: which sound plays for a StageCraft.Sound.* tag, and how loud/how often. */
USTRUCT()
struct MODULARSCENEBUILDER_API FStageAudioCue
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Cue", meta = (Categories = "StageCraft.Sound"))
	FGameplayTag CueTag;

	UPROPERTY(EditAnywhere, Category = "Cue")
	TSoftObjectPtr<class USoundBase> Sound;

	/** Authored level of this cue, multiplied by the user's Effects volume. */
	UPROPERTY(EditAnywhere, Category = "Cue", meta = (ClampMin = "0.0", ClampMax = "2.0"))
	float VolumeScale = 1.f;

	/** A retrigger sooner than this is dropped, so fast cursor sweeps over a grid do not machine-gun. */
	UPROPERTY(EditAnywhere, Category = "Cue", meta = (ClampMin = "0.0", Units = "s"))
	float MinRetriggerInterval = 0.04f;
};

/**
 * Project-wide feedback sound configuration (Project Settings > Game > StageCraft Audio, saved to
 * DefaultGame.ini). Designer data only; user volumes live in UStageCraftUserSettings.
 * Sounds referenced here must be cooked: /Game/StageCraft/Audio is in DirectoriesToAlwaysCook.
 */
UCLASS(Config = Game, DefaultConfig, meta = (DisplayName = "StageCraft Audio"))
class MODULARSCENEBUILDER_API UStageAudioDeveloperSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	//~ Begin UDeveloperSettings Interface
	virtual FName GetCategoryName() const override { return TEXT("Game"); }
	//~ End UDeveloperSettings Interface

	/** One entry per cue tag; a tag without an entry (or without a sound) is silent. */
	UPROPERTY(Config, EditAnywhere, Category = "Feedback", meta = (TitleProperty = "CueTag"))
	TArray<FStageAudioCue> Cues;
};
