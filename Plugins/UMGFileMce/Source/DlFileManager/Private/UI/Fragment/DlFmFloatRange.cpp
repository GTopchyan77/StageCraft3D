// Copyright(c) 2023 Dragon Li Software Limited. All Rights Reserved.


#include "UI/Fragment/DlFmFloatRange.h"

#include "Blueprint/SlateBlueprintLibrary.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "UI/Fragment/DlFmResizeDraggable_Logic.h"
#include "ModifyU/UI/DlFmBorder.h"
#include "ModifyU/UI/DlFmFloat_EditSimple.h"

void UDlFmFloatRange::NativeConstruct()
{
	Super::NativeConstruct();

	BorderMax_ParentSlot = Cast<UCanvasPanelSlot>(Border_Max->Slot);
	BorderMin_ParentSlot = Cast<UCanvasPanelSlot>(Border_Min->Slot);

	Border_Min->OnMouseButtonDownEvent.BindUFunction(this,TEXT("OnMouseButtonDown_Min"));
	Border_Max->OnMouseButtonDownEvent.BindUFunction(this,TEXT("OnMouseButtonDown_Max"));

	Float_Max->OnValueCommitted.AddUniqueDynamic(this, &UDlFmFloatRange::OnValueChanged_Max);
	Float_Min->OnValueCommitted.AddUniqueDynamic(this, &UDlFmFloatRange::OnValueChanged_Min);

	RefreshWidget_Max();
	RefreshWidget_Min();


	if(IsValid(Border_Max)&&(IsValid(Border_Min))&&(IsValid(Float_Max))&&(IsValid(Float_Min)))
	{
		bInit=true;
	}

	
}

int32 UDlFmFloatRange::NativePaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
                             FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle,
                             bool bParentEnabled) const
{
	
	if ((bInit)&& (AllottedGeometry != CacheSelfGeometry))
	{
		*CacheSelfGeometry_Ptr = AllottedGeometry;
		RefreshWidget_Max();
		RefreshWidget_Min();
	}
	return Super::NativePaint(Args, AllottedGeometry, MyCullingRect, OutDrawElements, LayerId, InWidgetStyle,
							  bParentEnabled);
}

FReply UDlFmFloatRange::NativeOnMouseMove(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent)
{
	if (InMouseEvent.IsMouseButtonDown(EKeys::LeftMouseButton))
	{
		FVector2D TmpPixelPosition, MouseViewportPosition;
		USlateBlueprintLibrary::AbsoluteToViewport(this,
		                                           InMouseEvent.GetScreenSpacePosition(), TmpPixelPosition,
		                                           MouseViewportPosition);
		if ((ActionType != EActionType::None) && (ActionSlot != nullptr))
		{
			switch (Orientation)
			{
			case EOrientation::Orient_Horizontal:
				{
					const float CanvasPosRange = Canvas_Background->GetCachedGeometry().Size.X;
					const float ExpectPosX = (MouseViewportPosition.X - PreMousePosition.X) /
						Canvas_Background->GetRenderTransform().Scale.X + PrePosition.X;
					const float ExpectValue = (bAllow_DragOutSliderRange)
						                          ? FMath::GetMappedRangeValueUnclamped(
							                          FVector2D(0.f, CanvasPosRange), SliderRange, ExpectPosX)
						                          : FMath::GetMappedRangeValueClamped(
							                          FVector2D(0.f, CanvasPosRange), SliderRange, ExpectPosX);

					switch (ActionType)
					{
					case EActionType::Move_Max:
						SetMax(ExpectValue, true);
						break;
					case EActionType::Move_Min:
						SetMin(ExpectValue, true);
						break;
					default:
						break;
					}
				}
				break;
			case EOrientation::Orient_Vertical:
				{
					const float CanvasPosRange = Canvas_Background->GetCachedGeometry().Size.Y;
					const float ExpectPosY = (MouseViewportPosition.Y - PreMousePosition.Y) /
						Canvas_Background->GetRenderTransform().Scale.Y + PrePosition.Y;
					const float ExpectValue = (bAllow_DragOutSliderRange)
						                          ? FMath::GetMappedRangeValueUnclamped(
							                          FVector2D(0.f, CanvasPosRange), SliderRange, ExpectPosY)
						                          : FMath::GetMappedRangeValueClamped(
							                          FVector2D(0.f, CanvasPosRange), SliderRange, ExpectPosY);

					switch (ActionType)
					{
					case EActionType::Move_Max:
						SetMax(ExpectValue, true);
						break;
					case EActionType::Move_Min:
						SetMin(ExpectValue, true);
						break;
					default:
						break;
					}
				}
				break;
			default:
				break;
			}
			return FReply::Handled();
		}
		return FReply::Unhandled();
	}
	else
	{
		if ((ActionType != EActionType::None) || (ActionSlot != nullptr))
		{
			FinishAction();
			return ReturnReleaseMouse(InMouseEvent);
		}
		else
		{
			return FReply::Unhandled();
		}
	}
}

FReply UDlFmFloatRange::NativeOnMouseButtonUp(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent)
{
	if (InMouseEvent.GetEffectingButton() == EKeys::LeftMouseButton)
	{
		FinishAction();
		return ReturnReleaseMouse(InMouseEvent);
	}
	else
	{
		return FReply::Unhandled();
	}
}

void UDlFmFloatRange::SetMax(const float Val, bool bBroadcast)
{
	const float TmpVal = (bAllow_ValueOutSliderRange)
		                     ? FMath::Max(Value.X + MinimumDelta, Val)
		                     : FMath::Clamp(FMath::Max(Value.X + MinimumDelta, Val), SliderRange.X, SliderRange.Y);
	if (TmpVal != Value.Y)
	{
		Value.Y = TmpVal;
		if (bBroadcast)
		{
			OnAreaChanged.Broadcast(Value);
		}
		RefreshWidget_Max();
	}
}

void UDlFmFloatRange::SetMin(float Val, bool bBroadcast)
{
	const float TmpVal = (bAllow_ValueOutSliderRange)
		                     ? FMath::Min(Val, Value.Y - MinimumDelta)
		                     : FMath::Clamp(FMath::Min(Val, Value.Y - MinimumDelta), SliderRange.X, SliderRange.Y);
	if (TmpVal != Value.X)
	{
		Value.X = TmpVal;
		if (bBroadcast)
		{
			OnAreaChanged.Broadcast(Value);
		}
		RefreshWidget_Min();
	}
}

void UDlFmFloatRange::SetValue(const FVector2D& Val, bool bBroadcast)
{
	if (((Val.Y - Val.X) >= MinimumDelta) && (Val != Value))
	{
		Value = Val;
		if (bBroadcast)
		{
			OnAreaChanged.Broadcast(Value);
		}
		RefreshWidget_Max();
		RefreshWidget_Min();
	}
}

void UDlFmFloatRange::SetSliderRange(const float& MinVal, const float& MaxVal)
{
	SetSliderRange(FVector2D(MinVal,MaxVal));
}

void UDlFmFloatRange::SetSliderRange(const FVector2D& Val)
{
	if (((Val.Y - Val.X) >= MinimumDelta) && (Val != SliderRange))
	{
		SliderRange = Val;
		RefreshWidget_Max();
		RefreshWidget_Min();
	}
}

FEventReply UDlFmFloatRange::OnMouseButtonDown_Min(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	if (MouseEvent.GetEffectingButton() == EKeys::LeftMouseButton)
	{
		StartAction(MouseEvent, EActionType::Move_Min);
		return ReturnCaptureMouse(MouseEvent);
	}
	else
	{
		return FEventReply(false);
	}
}

FEventReply UDlFmFloatRange::OnMouseButtonDown_Max(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	if (MouseEvent.GetEffectingButton() == EKeys::LeftMouseButton)
	{
		StartAction(MouseEvent, EActionType::Move_Max);
		return ReturnCaptureMouse(MouseEvent);
	}
	else
	{
		return FEventReply(false);
	}
}

void UDlFmFloatRange::OnValueChanged_Min(float Val, ETextCommit::Type CommitMethod)
{
	SetMin(Val, true);
}

void UDlFmFloatRange::OnValueChanged_Max(float Val, ETextCommit::Type CommitMethod)
{
	SetMax(Val, true);
}

FEventReply UDlFmFloatRange::ReturnCaptureMouse(const FPointerEvent& InMouseEvent)
{
	FEventReply EventReply = UWidgetBlueprintLibrary::DetectDragIfPressed(
		InMouseEvent, this, EKeys::LeftMouseButton);
	return UWidgetBlueprintLibrary::CaptureMouse(EventReply, this);
}

FReply UDlFmFloatRange::ReturnReleaseMouse(const FPointerEvent& InMouseEvent)
{
	FEventReply EventReply = UWidgetBlueprintLibrary::DetectDragIfPressed(
		InMouseEvent, this, EKeys::LeftMouseButton);
	return UWidgetBlueprintLibrary::ReleaseMouseCapture(EventReply).NativeReply;
}

void UDlFmFloatRange::StartAction(const FPointerEvent& InMouseEvent, EActionType Type)
{
	ActionType = Type;
	switch (ActionType)
	{
	case EActionType::Move_Max:
		ActionSlot = BorderMax_ParentSlot;
		break;
	case EActionType::Move_Min:
		ActionSlot = BorderMin_ParentSlot;
		break;
	default:
		return;
	}
	PrePosition = ActionSlot->GetPosition();
	PreSize = ActionSlot->GetSize();

	FVector2D TmpPixelPosition, MouseViewportPosition;
	USlateBlueprintLibrary::AbsoluteToViewport(this, InMouseEvent.GetScreenSpacePosition(), TmpPixelPosition,
	                                           MouseViewportPosition);
	PreMousePosition = MouseViewportPosition;
}

void UDlFmFloatRange::FinishAction()
{
	ActionType = EActionType::None;
	ActionSlot = nullptr;
}

void UDlFmFloatRange::RefreshWidget_Min() const
{
	Float_Min->SetValue_NoBroadcast(Value.X);

	FVector2D TmpPos = BorderMin_ParentSlot->GetPosition();
	switch (Orientation)
	{
	case EOrientation::Orient_Horizontal:
		TmpPos.X = FMath::GetMappedRangeValueClamped(
			SliderRange,
			FVector2D(0, Canvas_Background->GetCachedGeometry().Size.X),
			Value.X) - BorderMin_ParentSlot->GetSize().X / 2;
		break;
	case EOrientation::Orient_Vertical:
		TmpPos.Y = FMath::GetMappedRangeValueClamped(
			SliderRange,
			FVector2D(0, Canvas_Background->GetCachedGeometry().Size.Y),
			Value.X) - BorderMin_ParentSlot->GetSize().Y / 2;
		break;
	default:
		break;
	}
	BorderMin_ParentSlot->SetPosition(TmpPos);
}

void UDlFmFloatRange::RefreshWidget_Max() const
{
	Float_Max->SetValue_NoBroadcast(Value.Y);

	FVector2D TmpPos = BorderMax_ParentSlot->GetPosition();
	switch (Orientation)
	{
	case EOrientation::Orient_Horizontal:
		TmpPos.X = FMath::GetMappedRangeValueClamped(
			SliderRange,
			FVector2D(0, Canvas_Background->GetCachedGeometry().Size.X),
			Value.Y) - BorderMax_ParentSlot->GetSize().X / 2;
		break;
	case EOrientation::Orient_Vertical:
		TmpPos.Y = FMath::GetMappedRangeValueClamped(
			SliderRange,
			FVector2D(0, Canvas_Background->GetCachedGeometry().Size.Y),
			Value.Y) - BorderMax_ParentSlot->GetSize().Y / 2;
		break;
	default:
		break;
	}
	BorderMax_ParentSlot->SetPosition(TmpPos);
}
