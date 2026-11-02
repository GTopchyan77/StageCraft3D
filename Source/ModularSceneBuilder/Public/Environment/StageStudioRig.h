// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Environment/StageLevelLighting.h"
#include "GameFramework/Actor.h"
#include "StageStudioRig.generated.h"

/**
 * The "Studio" lighting source (Docs/ADR/0024-startup-atmospheres-starter-stages.md §4): a photo-studio look for showing a
 * stage design without a venue around it.
 *
 * - A movable sky light, from an HDRI cubemap when one is given (SetCubemap), otherwise from a flat neutral capture with a
 *   light lower hemisphere: the fallback never leaves the stage unlit.
 * - A soft key light from the front left, its intensity and colour from the look (ApplyLook).
 * - A light-grey height fog as a seamless backdrop, with volumetric fog so fixture beams still read.
 *
 * While it exists, the level's own lights, sky and fog are hidden (FStageHiddenLevelLighting) and shown again when it is
 * destroyed. Spawned and destroyed only by UStageEnvironmentSubsystem; transient, never saved with a level. Nothing ticks.
 */
UCLASS(NotPlaceable, Transient)
class MODULARSCENEBUILDER_API AStageStudioRig : public AActor
{
	GENERATED_BODY()

public:
	AStageStudioRig();

	//~ Begin AActor Interface
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	//~ End AActor Interface

	/** Lights with Cubemap, or with the flat fallback when it is null. */
	void SetCubemap(class UTextureCube* Cubemap);

	/** Key light intensity and colour from the look. */
	void ApplyLook(const struct FStageEnvironmentLook& Look);

	bool UsesHdri() const { return bUsesHdri; }

private:
	UPROPERTY(VisibleAnywhere, Category = "Studio")
	TObjectPtr<class USkyLightComponent> SkyLight = nullptr;

	UPROPERTY(VisibleAnywhere, Category = "Studio")
	TObjectPtr<class UDirectionalLightComponent> KeyLight = nullptr;

	UPROPERTY(VisibleAnywhere, Category = "Studio")
	TObjectPtr<class UExponentialHeightFogComponent> Backdrop = nullptr;

	FStageHiddenLevelLighting HiddenLevel;
	bool bUsesHdri = false;
};
