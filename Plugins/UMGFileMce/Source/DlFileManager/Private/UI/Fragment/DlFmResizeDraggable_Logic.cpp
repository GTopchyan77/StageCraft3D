// Copyright(c) 2023 Dragon Li Software Limited. All Rights Reserved.


#include "UI/Fragment/DlFmResizeDraggable_Logic.h"

#include "Blueprint/SlateBlueprintLibrary.h"
#include "Blueprint/WidgetBlueprintLibrary.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "ModifyU/UI/DlFmBorder.h"
#include "ModifyU/UI/DlFmButton.h"

void UDlFmResizeDraggable_Logic::OnInternalClick_Close()
{
	SetVisibility(ESlateVisibility::Collapsed);
}

void UDlFmResizeDraggable_Logic::FinishAllAction()
{
	bIsDragging=false;
	ResizeType=EResizeType::None;
}

void UDlFmResizeDraggable_Logic::StartAction_Drag(const FPointerEvent& InMouseEvent)
{
	bIsDragging=true;
	ResizeType=EResizeType::None;
	PrePosition=ParentCanvasSlot->GetPosition();
	PreSize=ParentCanvasSlot->GetSize();
	
	FVector2D TmpPixelPosition, MouseViewportPosition;
	USlateBlueprintLibrary::AbsoluteToViewport(this, InMouseEvent.GetScreenSpacePosition(), TmpPixelPosition,
											   MouseViewportPosition);
	PreMousePosition=MouseViewportPosition;
}

void UDlFmResizeDraggable_Logic::StartAction_Resize(const FPointerEvent& InMouseEvent, EResizeType Type)
{
	bIsDragging=false;
	ResizeType=Type;
	PrePosition=ParentCanvasSlot->GetPosition();
	PreSize=ParentCanvasSlot->GetSize();
	
	FVector2D TmpPixelPosition, MouseViewportPosition;
	USlateBlueprintLibrary::AbsoluteToViewport(this, InMouseEvent.GetScreenSpacePosition(), TmpPixelPosition,
											   MouseViewportPosition);
	PreMousePosition=MouseViewportPosition;
}

void UDlFmResizeDraggable_Logic::NativeConstruct()
{
	ParentCanvasSlot = Cast<UCanvasPanelSlot>(Slot);
	ParentCanvas = Cast<UCanvasPanel>(GetParent());

	//if parent is not canvas, do nothing
	if (ParentCanvasSlot != nullptr)
	{
		if (IsValid(Border_DragHandle))
		{
			//Border_DragHandle->OnMouseButtonUpEvent.BindUFunction(this, FName("OnMouseButtonUp_DragHandle"));
			Border_DragHandle-> OnMouseButtonDownEvent.BindUFunction(this, FName("OnMouseButtonDown_DragHandle"));
			Border_DragHandle->SetCursor(EMouseCursor::CardinalCross);
		}

		if (IsValid(Border_ResizeHandle_RightDown))
		{
			Border_ResizeHandle_RightDown->OnMouseButtonDownEvent.BindUFunction(
				this, FName("OnMouseButtonDown_ResizeHandle_RightDown"));
			Border_ResizeHandle_RightDown->SetCursor(EMouseCursor::ResizeSouthEast);
		}
		if (IsValid(Border_ResizeHandle_RightUp))
		{
			Border_ResizeHandle_RightUp->OnMouseButtonDownEvent.BindUFunction(
				this, FName("OnMouseButtonDown_ResizeHandle_RightUp"));
			Border_ResizeHandle_RightUp->SetCursor(EMouseCursor::ResizeSouthWest);
		}
		if (IsValid(Border_ResizeHandle_LeftDown))
		{
			Border_ResizeHandle_LeftDown->OnMouseButtonDownEvent.BindUFunction(
				this, FName("OnMouseButtonDown_ResizeHandle_LeftDown"));
			Border_ResizeHandle_LeftDown->SetCursor(EMouseCursor::ResizeSouthWest);
		}
		if (IsValid(Border_ResizeHandle_LeftUp))
		{
			Border_ResizeHandle_LeftUp->OnMouseButtonDownEvent.BindUFunction(
				this, FName("OnMouseButtonDown_ResizeHandle_LeftUp"));
			Border_ResizeHandle_LeftUp->SetCursor(EMouseCursor::ResizeSouthEast);
		}
		if (IsValid(Border_ResizeHandle_RightOnly))
		{
			Border_ResizeHandle_RightOnly->OnMouseButtonDownEvent.BindUFunction(
				this, FName("OnMouseButtonDown_ResizeHandle_RightOnly"));
			Border_ResizeHandle_RightOnly->SetCursor(EMouseCursor::ResizeLeftRight);
		}
		if (IsValid(Border_ResizeHandle_LeftOnly))
		{
			Border_ResizeHandle_LeftOnly->OnMouseButtonDownEvent.BindUFunction(
				this, FName("OnMouseButtonDown_ResizeHandle_LeftOnly"));
			Border_ResizeHandle_LeftOnly->SetCursor(EMouseCursor::ResizeLeftRight);
		}
		if (IsValid(Border_ResizeHandle_DownOnly))
		{
			Border_ResizeHandle_DownOnly->OnMouseButtonDownEvent.BindUFunction(
				this, FName("OnMouseButtonDown_ResizeHandle_DownOnly"));
			Border_ResizeHandle_DownOnly->SetCursor(EMouseCursor::ResizeUpDown);
		}
		if (IsValid(Border_ResizeHandle_UpOnly))
		{
			Border_ResizeHandle_UpOnly->OnMouseButtonDownEvent.BindUFunction(
				this, FName("OnMouseButtonDown_ResizeHandle_UpOnly"));
			Border_ResizeHandle_UpOnly->SetCursor(EMouseCursor::ResizeUpDown);
		}
	}

	if (IsValid(Button_Close))
	{
		Button_Close->OnFilteredClicked.AddUniqueDynamic(this, &UDlFmResizeDraggable_Logic::OnInternalClick_Close);
	}

	Super::NativeConstruct();
}

FReply UDlFmResizeDraggable_Logic::NativeOnMouseMove(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent)
{
	if(InMouseEvent.IsMouseButtonDown(EKeys::LeftMouseButton))
	{
		
		FVector2D TmpPixelPosition, MouseViewportPosition;
		USlateBlueprintLibrary::AbsoluteToViewport(this, InMouseEvent.GetScreenSpacePosition(), TmpPixelPosition,
												   MouseViewportPosition);
		if ((bIsDragging) && (ParentCanvasSlot != nullptr))
		{
			const FVector2D LogicDelta=(MouseViewportPosition-PreMousePosition)/ParentCanvas->GetRenderTransform().Scale;
			ParentCanvasSlot->SetPosition(PrePosition+LogicDelta);
			return FReply::Handled();
		}
		else if (ResizeType != EResizeType::None)
		{
			const FVector2D LogicDelta=(MouseViewportPosition-PreMousePosition)/ParentCanvas->GetRenderTransform().Scale;
			FVector2D TmpNewSize;
			FVector2D PositionDelta;
			Internal_DetermineNewSize(LogicDelta, TmpNewSize, PositionDelta);

			ParentCanvasSlot->SetPosition(PrePosition + PositionDelta);
			ParentCanvasSlot->SetSize(TmpNewSize);
			return FReply::Handled();
		}
		return FReply::Unhandled();
	}
	else
	{
		if((bIsDragging)||(ResizeType != EResizeType::None))
		{
			FinishAllAction();
			return ReturnReleaseMouse(InMouseEvent);
		}
		else
		{
			return FReply::Unhandled();
		}
	}
}

FReply UDlFmResizeDraggable_Logic::NativeOnMouseButtonUp(const FGeometry& InGeometry,
                                                            const FPointerEvent& InMouseEvent)
{
	if(InMouseEvent.GetEffectingButton()==EKeys::LeftMouseButton)
	{
		FinishAllAction();
		return ReturnReleaseMouse(InMouseEvent);
	}
	else
	{
		return FReply::Unhandled();
	}
}

void UDlFmResizeDraggable_Logic::Internal_DetermineNewSize(const FVector2D& InDelta, FVector2D& ResultSize,
                                                              FVector2D& PositionDelta) const
{
	FVector2D PlanningSize;
	switch (ResizeType)
	{
	case EResizeType::Resizing_RightDown:
		PlanningSize = PreSize + InDelta;
		Internal_SizeFilter(PlanningSize, ResultSize);
		PositionDelta = FVector2D::ZeroVector;
		break;
	case EResizeType::Resizing_LeftUp:
		PlanningSize = PreSize - InDelta;
		Internal_SizeFilter(PlanningSize, ResultSize);
		PositionDelta = PreSize - ResultSize;
		break;
	case EResizeType::Resizing_LeftDown:
		PlanningSize.X = PreSize.X - InDelta.X;
		PlanningSize.Y = PreSize.Y + InDelta.Y;
		Internal_SizeFilter(PlanningSize, ResultSize);
		PositionDelta = FVector2D(PreSize.X - ResultSize.X, 0);
		break;
	case EResizeType::Resizing_RightUp:
		PlanningSize.X = PreSize.X + InDelta.X;
		PlanningSize.Y = PreSize.Y - InDelta.Y;
		Internal_SizeFilter(PlanningSize, ResultSize);
		PositionDelta = FVector2D(0, PreSize.Y - ResultSize.Y);
		break;
	case EResizeType::Resizing_RightOnly:
		PlanningSize.X = PreSize.X + InDelta.X;
		PlanningSize.Y = PreSize.Y;
		Internal_SizeFilter(PlanningSize, ResultSize);
		PositionDelta = FVector2D::ZeroVector;
		break;
	case EResizeType::Resizing_LeftOnly:
		PlanningSize.X = PreSize.X - InDelta.X;
		PlanningSize.Y = PreSize.Y;
		Internal_SizeFilter(PlanningSize, ResultSize);
		PositionDelta = FVector2D(PreSize.X - ResultSize.X, 0);
		break;
	case EResizeType::Resizing_UpOnly:
		PlanningSize.X = PreSize.X;
		PlanningSize.Y = PreSize.Y - InDelta.Y;
		Internal_SizeFilter(PlanningSize, ResultSize);
		PositionDelta = FVector2D(0, PreSize.Y - ResultSize.Y);
		break;
	case EResizeType::Resizing_DownOnly:
		PlanningSize.X = PreSize.X;
		PlanningSize.Y = PreSize.Y + InDelta.Y;
		Internal_SizeFilter(PlanningSize, ResultSize);
		PositionDelta = FVector2D::ZeroVector;
		break;
	default:
		PlanningSize = PreSize;
		Internal_SizeFilter(PlanningSize, ResultSize);
		PositionDelta = FVector2D::ZeroVector;
		break;
	}
}

void UDlFmResizeDraggable_Logic::Internal_SizeFilter(const FVector2D& PlanningSize, FVector2D& AllowedSize) const
{
	AllowedSize = PlanningSize;
	if(bLimitResizeWidth)
	{
		AllowedSize.X=FMath::Clamp(PlanningSize.X,LimitResizeWidthArrange.Min,LimitResizeWidthArrange.Max);
	}
	if(bLimitResizeHeight)
	{
		AllowedSize.Y=FMath::Clamp(PlanningSize.Y,LimitResizeHeightArrange.Min,LimitResizeHeightArrange.Max);
	}
	if(bLimitToBiggerThanDesiredSize)
	{
		AllowedSize.X=FMath::Max(AllowedSize.X,GetDesiredSize().X);
		AllowedSize.Y=FMath::Max(AllowedSize.Y,GetDesiredSize().Y);
	}
}

FEventReply UDlFmResizeDraggable_Logic::ReturnCaptureMouse(const FPointerEvent& InMouseEvent)
{
	FEventReply EventReply = UWidgetBlueprintLibrary::DetectDragIfPressed(
		InMouseEvent, this, EKeys::LeftMouseButton);
	return UWidgetBlueprintLibrary::CaptureMouse(EventReply, this);
}

FReply UDlFmResizeDraggable_Logic::ReturnReleaseMouse(const FPointerEvent& InMouseEvent)
{
	FEventReply EventReply = UWidgetBlueprintLibrary::DetectDragIfPressed(
		InMouseEvent, this, EKeys::LeftMouseButton);
	return UWidgetBlueprintLibrary::ReleaseMouseCapture(EventReply).NativeReply;
}

FEventReply UDlFmResizeDraggable_Logic::OnMouseButtonDown_DragHandle(
	FGeometry InGeometry, const FPointerEvent& InMouseEvent)
{
	if(InMouseEvent.GetEffectingButton()==EKeys::LeftMouseButton)
	{
		
		StartAction_Drag(InMouseEvent);
		return ReturnCaptureMouse(InMouseEvent);
	}
	else
	{
		return FEventReply(false);
	}
}

FEventReply UDlFmResizeDraggable_Logic::OnMouseButtonDown_ResizeHandle_RightDown(FGeometry InGeometry,
	const FPointerEvent& InMouseEvent)
{
	if(InMouseEvent.GetEffectingButton()==EKeys::LeftMouseButton)
	{
		StartAction_Resize(InMouseEvent,EResizeType::Resizing_RightDown);
		return ReturnCaptureMouse(InMouseEvent);
	}
	else
	{
		return FEventReply(false);
	}
}

FEventReply UDlFmResizeDraggable_Logic::OnMouseButtonDown_ResizeHandle_RightUp(FGeometry InGeometry,
	const FPointerEvent& InMouseEvent)
{
	if(InMouseEvent.GetEffectingButton()==EKeys::LeftMouseButton)
	{
		StartAction_Resize(InMouseEvent,EResizeType::Resizing_RightUp);
		return ReturnCaptureMouse(InMouseEvent);
	}
	else
	{
		return FEventReply(false);
	}
}

FEventReply UDlFmResizeDraggable_Logic::OnMouseButtonDown_ResizeHandle_LeftUp(FGeometry InGeometry,
	const FPointerEvent& InMouseEvent)
{
	if(InMouseEvent.GetEffectingButton()==EKeys::LeftMouseButton)
	{
		StartAction_Resize(InMouseEvent,EResizeType::Resizing_LeftUp);
		return ReturnCaptureMouse(InMouseEvent);
	}
	else
	{
		return FEventReply(false);
	}
}

FEventReply UDlFmResizeDraggable_Logic::OnMouseButtonDown_ResizeHandle_LeftDown(FGeometry InGeometry,
	const FPointerEvent& InMouseEvent)
{
	if(InMouseEvent.GetEffectingButton()==EKeys::LeftMouseButton)
	{
		StartAction_Resize(InMouseEvent,EResizeType::Resizing_LeftDown);
		return ReturnCaptureMouse(InMouseEvent);
	}
	else
	{
		return FEventReply(false);
	}
}

FEventReply UDlFmResizeDraggable_Logic::OnMouseButtonDown_ResizeHandle_RightOnly(FGeometry InGeometry,
	const FPointerEvent& InMouseEvent)
{
	if(InMouseEvent.GetEffectingButton()==EKeys::LeftMouseButton)
	{
		StartAction_Resize(InMouseEvent,EResizeType::Resizing_RightOnly);
		return ReturnCaptureMouse(InMouseEvent);
	}
	else
	{
		return FEventReply(false);
	}
}

FEventReply UDlFmResizeDraggable_Logic::OnMouseButtonDown_ResizeHandle_DownOnly(FGeometry InGeometry,
	const FPointerEvent& InMouseEvent)
{
	if(InMouseEvent.GetEffectingButton()==EKeys::LeftMouseButton)
	{
		StartAction_Resize(InMouseEvent,EResizeType::Resizing_DownOnly);
		return ReturnCaptureMouse(InMouseEvent);
	}
	else
	{
		return FEventReply(false);
	}
}

FEventReply UDlFmResizeDraggable_Logic::OnMouseButtonDown_ResizeHandle_LeftOnly(FGeometry InGeometry,
	const FPointerEvent& InMouseEvent)
{
	if(InMouseEvent.GetEffectingButton()==EKeys::LeftMouseButton)
	{
		StartAction_Resize(InMouseEvent,EResizeType::Resizing_LeftOnly);
		return ReturnCaptureMouse(InMouseEvent);
	}
	else
	{
		return FEventReply(false);
	}
}

FEventReply UDlFmResizeDraggable_Logic::OnMouseButtonDown_ResizeHandle_UpOnly(FGeometry InGeometry,
	const FPointerEvent& InMouseEvent)
{
	if(InMouseEvent.GetEffectingButton()==EKeys::LeftMouseButton)
	{
		StartAction_Resize(InMouseEvent,EResizeType::Resizing_UpOnly);
		return ReturnCaptureMouse(InMouseEvent);
	}
	else
	{
		return FEventReply(false);
	}
}