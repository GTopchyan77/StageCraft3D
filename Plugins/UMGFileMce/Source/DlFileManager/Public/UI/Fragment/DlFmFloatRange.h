// Copyright(c) 2023 Dragon Li Software Limited. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetBlueprintLibrary.h"
#include "DlFmFloatRange.generated.h"

class UCanvasPanelSlot;
class UCanvasPanel;
class UDlFmBorder;
class UDlFmFloat_EditSimple;
/**
 * 
 */

UCLASS()
class DLFILEMANAGER_API UDlFmFloatRange : public UUserWidget
{
	GENERATED_BODY()
public:
	DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnAreaChanged, const FVector2D&, Value);

	FOnAreaChanged OnAreaChanged;

	virtual void NativeConstruct() override;
	virtual int32 NativePaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry,
	                          const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements, int32 LayerId,
	                          const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override;
	virtual FReply NativeOnMouseMove(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent) override;
	virtual FReply NativeOnMouseButtonUp(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent) override;

	void SetMax(float Val, bool bBroadcast);
	void SetMin(float Val, bool bBroadcast);
	void SetValue(const FVector2D& Val, bool bBroadcast);
	void SetSliderRange(const float& MinVal, const float& MaxVal);
	void SetSliderRange(const FVector2D& Val);

	UPROPERTY(EditAnywhere,meta=(ClampMin=0), Category="Behavior")
	float MinimumDelta=0;
	 
	UPROPERTY(EditAnywhere, Category="Behavior")
	bool bAllow_DragOutSliderRange=false;
	UPROPERTY(EditAnywhere, Category="Behavior")
	bool bAllow_ValueOutSliderRange=false;
private:
	enum class EActionType:uint8
	{
		None,
		Move_Min,
		Move_Max,
	};

	EActionType ActionType = EActionType::None;

	UPROPERTY(EditAnywhere, Category="Behavior")
	TEnumAsByte<EOrientation> Orientation;
	UPROPERTY(EditAnywhere, Category="Behavior")
	FVector2D SliderRange = FVector2D(0, 1);
	UPROPERTY(EditAnywhere, Category="Behavior")
	FVector2D Value = FVector2D(0.25, 0.75);

	UPROPERTY(meta = (BindWidget))
	UCanvasPanel* Canvas_Background;
	UPROPERTY(meta = (BindWidget))
	UDlFmBorder* Border_Min;
	UFUNCTION()
	FEventReply OnMouseButtonDown_Min(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent);
	UPROPERTY(meta = (BindWidget))
	UDlFmBorder* Border_Max;
	UFUNCTION()
	FEventReply OnMouseButtonDown_Max(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent);
	UPROPERTY(meta = (BindWidget))
	UDlFmFloat_EditSimple* Float_Min;
	UFUNCTION()
	void OnValueChanged_Min(float Val, ETextCommit::Type CommitMethod);
	UPROPERTY(meta = (BindWidget))
	UDlFmFloat_EditSimple* Float_Max;
	UFUNCTION()
	void OnValueChanged_Max(float Val, ETextCommit::Type CommitMethod);


	UPROPERTY()
	UCanvasPanelSlot* BorderMin_ParentSlot;
	UPROPERTY()
	UCanvasPanelSlot* BorderMax_ParentSlot;


	FEventReply ReturnCaptureMouse(const FPointerEvent& InMouseEvent);
	FReply ReturnReleaseMouse(const FPointerEvent& InMouseEvent);

	void StartAction(const FPointerEvent& InMouseEvent, EActionType Type);
	void FinishAction();
	void RefreshWidget_Min() const;
	void RefreshWidget_Max() const;
	UCanvasPanelSlot* ActionSlot;
	FVector2D PrePosition, PreSize, PreMousePosition;
	FGeometry CacheSelfGeometry;
	FGeometry* CacheSelfGeometry_Ptr=&CacheSelfGeometry;

	bool bInit=false;

};
