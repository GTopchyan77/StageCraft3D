// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Camera/CameraModifier.h"
#include "StageLineViewCameraModifier.generated.h"

/**
 * Adds the CAD line-view post-process material to the player's view (Docs/ADR/0008 §4.3).
 *
 * A camera modifier's post-process goes to the camera manager's blend cache, which reaches the player's viewport but not
 * GetCameraCacheView().PostProcessSettings, which still renders copy (ADR 0008 F7). So the line view never leaks into a
 * render. UStageViewportComponent creates it, enables it only in Lines mode and disables it otherwise, so the camera
 * manager skips it entirely outside that mode.
 */
UCLASS()
class MODULARSCENEBUILDER_API UStageLineViewCameraModifier : public UCameraModifier
{
	GENERATED_BODY()

public:
	UStageLineViewCameraModifier();

	/** The post-process material to blend in. Null makes the modifier contribute nothing. */
	void SetMaterial(class UMaterialInterface* InMaterial) { Material = InMaterial; }

protected:
	//~ Begin UCameraModifier Interface
	virtual void ModifyPostProcess(float DeltaTime, float& PostProcessBlendWeight, FPostProcessSettings& PostProcessSettings) override;
	//~ End UCameraModifier Interface

private:
	UPROPERTY(Transient)
	TObjectPtr<class UMaterialInterface> Material = nullptr;
};
