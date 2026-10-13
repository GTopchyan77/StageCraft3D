// Copyright(c) 2023 Dragon Li Software Limited. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "DlFmResizeDraggable_Logic.generated.h"

class UDlFmBorder;
class UScrollBox;
class UCanvasPanel;
class UCanvasPanelSlot;
class UDlFmButton;
/**
 * 
 */
UCLASS()
class DLFILEMANAGER_API UDlFmResizeDraggable_Logic : public UUserWidget
{
	GENERATED_BODY()

private:
	UPROPERTY(EditAnywhere, Category = "Resize Arrange")
	bool bLimitResizeWidth=false;
	UPROPERTY(EditAnywhere, Category = "Resize Arrange",meta=(EditConditio=bLimitResizeWidth))
	FFloatInterval LimitResizeWidthArrange = FFloatInterval(100,200);
	UPROPERTY(EditAnywhere, Category = "Resize Arrange")
	bool bLimitResizeHeight=false;
	UPROPERTY(EditAnywhere, Category = "Resize Arrange",meta=(EditConditio=bLimitResizeWidth))
	FFloatInterval LimitResizeHeightArrange = FFloatInterval(100,200);
	UPROPERTY(EditAnywhere, Category = "Resize Arrange")
	bool bLimitToBiggerThanDesiredSize=false;

	UPROPERTY(meta = (BindWidgetOptional))
	UDlFmBorder* Border_DragHandle;
	UPROPERTY(meta = (BindWidgetOptional))
	UDlFmBorder* Border_ResizeHandle_RightDown;
	UPROPERTY(meta = (BindWidgetOptional))
	UDlFmBorder* Border_ResizeHandle_RightUp;
	UPROPERTY(meta = (BindWidgetOptional))
	UDlFmBorder* Border_ResizeHandle_LeftDown;
	UPROPERTY(meta = (BindWidgetOptional))
	UDlFmBorder* Border_ResizeHandle_LeftUp;
	UPROPERTY(meta = (BindWidgetOptional))
	UDlFmBorder* Border_ResizeHandle_LeftOnly;
	UPROPERTY(meta = (BindWidgetOptional))
	UDlFmBorder* Border_ResizeHandle_RightOnly;
	UPROPERTY(meta = (BindWidgetOptional))
	UDlFmBorder* Border_ResizeHandle_UpOnly;
	UPROPERTY(meta = (BindWidgetOptional))
	UDlFmBorder* Border_ResizeHandle_DownOnly;

	UPROPERTY(meta = (BindWidgetOptional))
	UDlFmButton* Button_Close;
	UFUNCTION()
	void OnInternalClick_Close();

	/** Parent Canvas Panel of this widget. Should not be null because dragging and resizing can ONLY be done if this widget is a child of Canvas Panel. */
	UPROPERTY()
	UCanvasPanelSlot* ParentCanvasSlot;

	enum class EResizeType:uint8
	{
		None,
		Resizing_RightDown,
		Resizing_RightUp,
		Resizing_LeftDown,
		Resizing_LeftUp,
		Resizing_LeftOnly,
		Resizing_RightOnly,
		Resizing_UpOnly,
		Resizing_DownOnly,
	};

	bool bIsDragging = false;
	EResizeType ResizeType = EResizeType::None;
	FVector2D PrePosition;
	FVector2D PreSize;
	FVector2D PreMousePosition;

	void FinishAllAction();
	void StartAction_Drag(const FPointerEvent& InMouseEvent); 
	void StartAction_Resize(const FPointerEvent& InMouseEvent, EResizeType Type); 

public:
	
	UPROPERTY()
	UCanvasPanel* ParentCanvas;
protected:
	virtual void NativeConstruct() override;
	virtual FReply NativeOnMouseMove(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent) override;
	virtual FReply NativeOnMouseButtonUp(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent) override;
private:
	void Internal_DetermineNewSize(const FVector2D& InDelta, FVector2D& ResultSize, FVector2D& PositionDelta) const;
	void Internal_SizeFilter(const FVector2D& PlanningSize, FVector2D& AllowedSize) const;
	FEventReply ReturnCaptureMouse(const FPointerEvent& InMouseEvent);
	FReply ReturnReleaseMouse(const FPointerEvent& InMouseEvent); 
protected:

	/** Event called when mouse button down is detected on DragHandle */
	UFUNCTION()
	FEventReply OnMouseButtonDown_DragHandle(FGeometry InGeometry, const FPointerEvent& InMouseEvent);

	/** Event called when mouse button down is detected on ResizeHandleWidget */
	UFUNCTION()
	FEventReply OnMouseButtonDown_ResizeHandle_RightDown(FGeometry InGeometry, const FPointerEvent& InMouseEvent);
	UFUNCTION()
	FEventReply OnMouseButtonDown_ResizeHandle_RightUp(FGeometry InGeometry, const FPointerEvent& InMouseEvent);
	UFUNCTION()
	FEventReply OnMouseButtonDown_ResizeHandle_LeftUp(FGeometry InGeometry, const FPointerEvent& InMouseEvent);
	UFUNCTION()
	FEventReply OnMouseButtonDown_ResizeHandle_LeftDown(FGeometry InGeometry, const FPointerEvent& InMouseEvent);
	UFUNCTION()
	FEventReply OnMouseButtonDown_ResizeHandle_RightOnly(FGeometry InGeometry, const FPointerEvent& InMouseEvent);
	UFUNCTION()
	FEventReply OnMouseButtonDown_ResizeHandle_DownOnly(FGeometry InGeometry, const FPointerEvent& InMouseEvent);
	UFUNCTION()
	FEventReply OnMouseButtonDown_ResizeHandle_LeftOnly(FGeometry InGeometry, const FPointerEvent& InMouseEvent);
	UFUNCTION()
	FEventReply OnMouseButtonDown_ResizeHandle_UpOnly(FGeometry InGeometry, const FPointerEvent& InMouseEvent);

};
