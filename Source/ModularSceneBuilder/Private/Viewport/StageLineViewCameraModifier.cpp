// Copyright Epic Games, Inc. All Rights Reserved.

#include "Viewport/StageLineViewCameraModifier.h"

#include "Materials/MaterialInterface.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageLineViewCameraModifier)

UStageLineViewCameraModifier::UStageLineViewCameraModifier()
{
	// A view mode switches at once; no fade in or out.
	AlphaInTime = 0.f;
	AlphaOutTime = 0.f;
}

void UStageLineViewCameraModifier::ModifyPostProcess(float DeltaTime, float& PostProcessBlendWeight, FPostProcessSettings& PostProcessSettings)
{
	if (!Material)
	{
		PostProcessBlendWeight = 0.f;
		return;
	}
	PostProcessBlendWeight = 1.f;
	PostProcessSettings.AddBlendable(Material, 1.f);
}
