// Copyright Epic Games, Inc. All Rights Reserved.

#include "Player/StageCraftGameViewportClient.h"

#include "InputCoreTypes.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageCraftGameViewportClient)

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
