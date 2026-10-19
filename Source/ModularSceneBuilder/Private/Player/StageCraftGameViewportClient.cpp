// Copyright Epic Games, Inc. All Rights Reserved.

#include "Player/StageCraftGameViewportClient.h"

#include "InputCoreTypes.h"
#include "ModularSceneBuilder.h"
#include "ShowFlags.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageCraftGameViewportClient)

void UStageCraftGameViewportClient::ApplyStageViewMode(EStageViewMode Mode)
{
	if (!BaseShowFlags.IsSet())
	{
		BaseShowFlags = EngineShowFlags;
	}

	FEngineShowFlags Flags = BaseShowFlags.GetValue();
	EViewModeIndex Index = VMI_Lit;
	switch (Mode)
	{
	case EStageViewMode::Unlit:
		Index = VMI_Unlit;
		ApplyViewMode(Index, /*bPerspective*/ true, Flags);
		break;

	case EStageViewMode::Lines:
		Index = VMI_Unlit;
		ApplyViewMode(Index, /*bPerspective*/ true, Flags);
		// ApplyViewMode turns post-processing off for Unlit; the line material is a post-process (ADR 0008 F6).
		Flags.SetPostProcessing(true);
		// Scene colour keeps only emissive surfaces and the grid lines; the material draws the outlines over a flat background.
		Flags.SetUnlitViewmode(false);
		// The material compares neighbouring depth and normal texels: TAA jitter would make the lines crawl.
		Flags.SetTemporalAA(false);
		Flags.SetAntiAliasing(false);
		Flags.SetEyeAdaptation(false);
		Flags.SetBloom(false);
		Flags.SetMotionBlur(false);
		break;

	case EStageViewMode::Lit:
	default:
		ApplyViewMode(Index, /*bPerspective*/ true, Flags);
		break;
	}

	EngineShowFlags = Flags;
	// Not SetViewMode: it forces Lit in Shipping and cooked builds (ADR 0008 F1). Draw reads ViewModeIndex every frame.
	ViewModeIndex = Index;
	StageViewMode = Mode;
	UE_LOG(LogStageCraft, Log, TEXT("Viewport view mode %s."), *UEnum::GetValueAsString(Mode));
}

bool UStageCraftGameViewportClient::InputKey(const FInputKeyEventArgs& EventArgs)
{
	// Tracked before Super, which may early-out (console open, input ignored); the button state must stay truthful.
	if (EventArgs.Key.IsMouseButton())
	{
		const bool bDown = EventArgs.Event == IE_Pressed || EventArgs.Event == IE_DoubleClick;
		const bool bUp = EventArgs.Event == IE_Released;

		if (bDown || bUp)
		{
			if (EventArgs.Key == EKeys::RightMouseButton)
			{
				bNavigationButtonDown = bDown;
			}
			else if (EventArgs.Key == EKeys::LeftMouseButton || EventArgs.Key == EKeys::MiddleMouseButton)
			{
				bOtherMouseButtonDown = bDown;
			}
		}
	}

	return Super::InputKey(EventArgs);
}

bool UStageCraftGameViewportClient::HideCursorDuringCapture() const
{
	// Queried by the viewport right after InputKey for the same mouse-down. A right press during a
	// left drag must not hide the cursor the drag is following.
	if (bNavigationButtonDown && !bOtherMouseButtonDown)
	{
		return true;
	}
	return Super::HideCursorDuringCapture();
}

void UStageCraftGameViewportClient::LostFocus(FViewport* InViewport)
{
	// Button-up events are not delivered once focus is gone (alt-tab mid-drag).
	bNavigationButtonDown = false;
	bOtherMouseButtonDown = false;
	Super::LostFocus(InViewport);
}

void UStageCraftGameViewportClient::MouseMove(FViewport* InViewport, int32 X, int32 Y)
{
	Super::MouseMove(InViewport, X, Y);
	OnCursorMoved.Broadcast();
}

void UStageCraftGameViewportClient::CapturedMouseMove(FViewport* InViewport, int32 X, int32 Y)
{
	Super::CapturedMouseMove(InViewport, X, Y);
	OnCursorMoved.Broadcast();
}
