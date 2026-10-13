// Copyright(c) 2023 Dragon Li Software Limited. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UI/Fragment/DlFmJoystick_Lib.h"
#include "Widgets/SLeafWidget.h"

/**
 * 
 */
class DLFILEMANAGER_API SDlFmJoystick : public SLeafWidget
{
public:

	SLATE_BEGIN_ARGS(SDlFmJoystick)
		:_GeneralConfig()
		{
		}
	SLATE_ARGUMENT(FDlFmJoystick_Config, GeneralConfig)
	SLATE_END_ARGS()

	
	struct FDlFmJoystick_Draft
	{
	public:
		struct FDraggingStatus
		{
			EDlFmJoystick_ControlSource ControlSource;
			FKey MouseKey;
			int32 TouchIndex;
			bool TryCapture(const EDlFmJoystick_ControlSource Src, const FPointerEvent& InEvent, const SDlFmJoystick* Env);
			bool IsCapturedBy(const EDlFmJoystick_ControlSource Src, const FPointerEvent& InEvent) const;
			bool IsCaptured() const; 
			bool IsNotCaptured() const;
			void ReleaseCapture();
		};

		struct FHandleDraft
		{
			FDraggingStatus DraggingStatus;
			FVector2D CurVal;
			FVector2D InterpTo;
			FVector2D ActualSize;
			FVector2D ActualPosition;
			FOnDlFmJoystick_HandleChanged OnHandle0Changed;

			
			void Set_CtrlSrc(EDlFmJoystick_ControlSource Val);
			void Set_Percent(const FVector2D& ValueVal,SDlFmJoystick* Env,const FDlFmJoystick_HandleConfig& HConfig); 
			void Set_PercentUnClamp_NoBroadcast(const FVector2D& ValueVal,SDlFmJoystick* Env);
			void SmoothedTo(const FVector2D& Val);
			void Tick(float DeltaTime,SDlFmJoystick* Env,const FDlFmJoystick_HandleConfig& HConfig);
			void OnPaint(FSlateWindowElementList& ElementList,uint32 InLayer, const FGeometry& AllottedGeometry,const FDlFmJoystick_HandleConfig& HConfig) const;
		};

		FHandleDraft PlayerHandle;
		FHandleDraft AIHandle;
	
		FVector2D ReachableArea_HalfSize;
		FVector2D ReachableArea_Center;

		FGeometry Cache_SelfGeometry;
		FMargin TouchableMargin;
		float Cache_MaxRadius;
	};

	void Construct(const FArguments& InArgs);
	virtual FVector2D ComputeDesiredSize(float) const override;

	FDlFmJoystick_Config Config;

	virtual void Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime) override;
	virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override;

	virtual FReply OnMouseButtonDown(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
	virtual FReply OnTouchStarted(const FGeometry& MyGeometry, const FPointerEvent& InTouchEvent) override;
	virtual FReply OnMouseMove(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
	virtual FReply OnTouchMoved(const FGeometry& MyGeometry, const FPointerEvent& InTouchEvent) override;
	virtual FReply OnMouseButtonUp(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
	virtual FReply OnTouchEnded(const FGeometry& MyGeometry, const FPointerEvent& InTouchEvent) override;

	void SetCtrlSrc_PlayerHandle(EDlFmJoystick_ControlSource Val){Draft.PlayerHandle.Set_CtrlSrc(Val);}
	void SetCtrlSrc_AIHandle(EDlFmJoystick_ControlSource Val){Draft.AIHandle.Set_CtrlSrc(Val);}
	void SmoothedTo_AIHandle(const FVector2D& Val){Draft.AIHandle.SmoothedTo(Val);}
	void Set_PercentUnClamp_NoBroadcast_PlayerHandle(const FVector2D& Val){Draft.PlayerHandle.Set_PercentUnClamp_NoBroadcast(Val,this);}
	FDlFmJoystick_Draft& GetDraft(){return Draft;}
private:
	
	FDlFmJoystick_Draft Draft=FDlFmJoystick_Draft();
	
	void SetHandle0_Event(const FGeometry& InGeometry, const FPointerEvent& InEvent);
	
	inline FVector2D ValuePercent2WidgetPercent_CheckReverse(const FVector2D& Val) const
	{
		return FVector2D(FMath::Clamp((Config.Common.bReverseX?-Val.X:Val.X),-1,1),
			FMath::Clamp((Config.Common.bReverseY? -Val.Y:Val.Y),-1,1));
	}
	inline FVector2D WidgetPercent2ValuePercent_CheckReverse(const FVector2D& Val) const
	{
		//for bool action, transform is same
		return ValuePercent2WidgetPercent_CheckReverse(Val);
	}
	void RefreshGeometry(const FGeometry& SelfGeometry);
	void SetHandlePosition(const FVector2D& ValuePercent,FVector2D& ActualPosition) const;
};


