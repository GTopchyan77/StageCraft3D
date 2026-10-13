// Copyright(c) 2023 Dragon Li Software Limited. All Rights Reserved.


#include "UI/Fragment/SDlFmJoystick.h"

#include "SlateOptMacros.h"

BEGIN_SLATE_FUNCTION_BUILD_OPTIMIZATION

void SDlFmJoystick::Construct(const FArguments& InArgs)
{
	Config = InArgs._GeneralConfig;
}

FVector2D SDlFmJoystick::ComputeDesiredSize(float) const
{
	return FVector2D(200, 200);
}

void SDlFmJoystick::Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime)
{
	SLeafWidget::Tick(AllottedGeometry, InCurrentTime, InDeltaTime);
	RefreshGeometry(AllottedGeometry);

	Draft.PlayerHandle.Tick(InDeltaTime,this,Config.PlayerHandle);
	Draft.AIHandle.Tick(InDeltaTime,this,Config.AIHandle);
}

int32 SDlFmJoystick::OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
	FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle,
	bool bParentEnabled) const
{
	const int32 TouchableArea_LayerId = LayerId + 1;
	const int32 ReachableArea_LayerId = LayerId + 2;
	const int32 PlayerHandle_LayerId = LayerId + 3;
	const int32 AIHandle_LayerId = LayerId + 4;

	Config.Common.BackgroundDrawBox.OnPaint(OutDrawElements,TouchableArea_LayerId,AllottedGeometry);
	Config.ReachableArea.DrawBox.OnPaint(OutDrawElements,ReachableArea_LayerId,AllottedGeometry,
		Draft.ReachableArea_Center-Draft.ReachableArea_HalfSize,
		Draft.ReachableArea_HalfSize*2);
	Draft.PlayerHandle.OnPaint(OutDrawElements,PlayerHandle_LayerId,AllottedGeometry,Config.PlayerHandle);
	Draft.AIHandle.OnPaint(OutDrawElements,AIHandle_LayerId,AllottedGeometry,Config.AIHandle);

	return LayerId;
}

FReply SDlFmJoystick::OnMouseButtonDown(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	if (Draft.PlayerHandle.DraggingStatus.TryCapture(EDlFmJoystick_ControlSource::InputEvent_Mouse, MouseEvent, this))
	{
		SetHandle0_Event(MyGeometry, MouseEvent);
		return FReply::Handled().CaptureMouse(SharedThis(this));
	}
	return FReply::Unhandled();
}

FReply SDlFmJoystick::OnTouchStarted(const FGeometry& MyGeometry, const FPointerEvent& InTouchEvent)
{
	if (Draft.PlayerHandle.DraggingStatus.TryCapture(EDlFmJoystick_ControlSource::InputEvent_Touch, InTouchEvent, this))
	{
		SetHandle0_Event(MyGeometry, InTouchEvent);
		return FReply::Handled().CaptureMouse(SharedThis(this));
	}
	return FReply::Unhandled();
}

FReply SDlFmJoystick::OnMouseMove(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	if (Draft.PlayerHandle.DraggingStatus.IsCapturedBy(EDlFmJoystick_ControlSource::InputEvent_Mouse, MouseEvent))
	{
		SetHandle0_Event(MyGeometry, MouseEvent);
		return FReply::Handled();
	}
	return FReply::Unhandled();
}

FReply SDlFmJoystick::OnTouchMoved(const FGeometry& MyGeometry, const FPointerEvent& InTouchEvent)
{
	if (Draft.PlayerHandle.DraggingStatus.IsCapturedBy(EDlFmJoystick_ControlSource::InputEvent_Touch, InTouchEvent))
	{
		SetHandle0_Event(MyGeometry, InTouchEvent);
		return FReply::Handled();
	}
	return FReply::Unhandled();
}

FReply SDlFmJoystick::OnMouseButtonUp(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	if (Draft.PlayerHandle.DraggingStatus.IsCapturedBy(EDlFmJoystick_ControlSource::InputEvent_Mouse, MouseEvent))
	{
		SetHandle0_Event(MyGeometry, MouseEvent);
		Draft.PlayerHandle.DraggingStatus.ReleaseCapture();
		return FReply::Handled().ReleaseMouseCapture();
	}
	return FReply::Unhandled();
}

FReply SDlFmJoystick::OnTouchEnded(const FGeometry& MyGeometry, const FPointerEvent& InTouchEvent)
{
	if (Draft.PlayerHandle.DraggingStatus.IsCapturedBy(EDlFmJoystick_ControlSource::InputEvent_Touch, InTouchEvent))
	{
		SetHandle0_Event(MyGeometry, InTouchEvent);
		Draft.PlayerHandle.DraggingStatus.ReleaseCapture();
		return FReply::Handled().ReleaseMouseCapture();
	}
	return FReply::Unhandled();
}

bool SDlFmJoystick::FDlFmJoystick_Draft::FDraggingStatus::TryCapture(const EDlFmJoystick_ControlSource Src,
	const FPointerEvent& InEvent, const SDlFmJoystick* Env)
{
	switch (Src)
	{
	case EDlFmJoystick_ControlSource::InputEvent_Mouse:
		if(Env->Config.Player.bAcceptInput_Mouse==false){return false;}
	case EDlFmJoystick_ControlSource::InputEvent_Touch:
		if(Env->Config.Player.bAcceptInput_Touch==false){return false;}
		default:
			break;
	}
	if (ControlSource == EDlFmJoystick_ControlSource::InputEvent_Waiting)
	{
		ControlSource = Src;
		MouseKey = InEvent.GetEffectingButton();
		TouchIndex = InEvent.GetPointerIndex();

		return true;
	}
	else
	{
		return false;
	}
}

bool SDlFmJoystick::FDlFmJoystick_Draft::FDraggingStatus::IsCapturedBy(const EDlFmJoystick_ControlSource Src,
	const FPointerEvent& InEvent) const
{
	if (ControlSource == Src)
	{
		switch (ControlSource)
		{
		case EDlFmJoystick_ControlSource::InputEvent_Touch:
			return (InEvent.GetPointerIndex() == TouchIndex);
		case EDlFmJoystick_ControlSource::InputEvent_Mouse:
			return (InEvent.GetEffectingButton() == MouseKey);
		default:
			return false;
		}
	}
	return false;
}

bool SDlFmJoystick::FDlFmJoystick_Draft::FDraggingStatus::IsCaptured() const
{
	return (ControlSource != EDlFmJoystick_ControlSource::InputEvent_Waiting);
}

bool SDlFmJoystick::FDlFmJoystick_Draft::FDraggingStatus::IsNotCaptured() const
{
	return (ControlSource == EDlFmJoystick_ControlSource::InputEvent_Waiting);
}

void SDlFmJoystick::FDlFmJoystick_Draft::FDraggingStatus::ReleaseCapture()
{
	ControlSource=EDlFmJoystick_ControlSource::InputEvent_Waiting;
}

void SDlFmJoystick::FDlFmJoystick_Draft::FHandleDraft::Set_CtrlSrc(EDlFmJoystick_ControlSource Val)
{
	DraggingStatus.ControlSource=Val;
}

void SDlFmJoystick::FDlFmJoystick_Draft::FHandleDraft::Set_Percent(const FVector2D& ValueVal,SDlFmJoystick* Env,const FDlFmJoystick_HandleConfig& HConfig)
{
	const FVector2D TmpHandle0Val = Env->Config.ReachableArea.ClampPercent(ValueVal);
	if(TmpHandle0Val!=CurVal)
	{
		Set_PercentUnClamp_NoBroadcast(TmpHandle0Val,Env);
		if(HConfig.bBroadcastOnEvent)
		{
			OnHandle0Changed.Broadcast(CurVal);
		}
	}
}

void SDlFmJoystick::FDlFmJoystick_Draft::FHandleDraft::Set_PercentUnClamp_NoBroadcast(const FVector2D& ValueVal,
	SDlFmJoystick* Env)
{
	CurVal = ValueVal;
	Env->SetHandlePosition(CurVal, ActualPosition);
}

void SDlFmJoystick::FDlFmJoystick_Draft::FHandleDraft::SmoothedTo(const FVector2D& Val)
{
	DraggingStatus.ControlSource=EDlFmJoystick_ControlSource::ProgramSet;
	InterpTo=Val;
}

void SDlFmJoystick::FDlFmJoystick_Draft::FHandleDraft::Tick(float DeltaTime,SDlFmJoystick* Env,const FDlFmJoystick_HandleConfig& HConfig)
{
	
	switch (DraggingStatus.ControlSource)
	{
	case EDlFmJoystick_ControlSource::InputEvent_Waiting:
		if (HConfig.bAutoBack_WhenNoControl)
		{
			if(CurVal!=HConfig.AutoBackPosition)
			{
				Set_Percent(FMath::Vector2DInterpTo(CurVal,
				HConfig.AutoBackPosition, DeltaTime, HConfig.AutoBackSpeed),Env,HConfig);
				if(HConfig.bBroadcastOnTick)
				{
					OnHandle0Changed.Broadcast(CurVal);
				}
			}
		}
		break;
	case EDlFmJoystick_ControlSource::ProgramSet:
		{
			if(HConfig.bLimitMoveSpeed)
			{
				const FVector2D TmpV = FMath::Vector2DInterpTo(
					CurVal, InterpTo, DeltaTime, HConfig.MaxMoveSpeed);
				Set_Percent(TmpV,Env,HConfig);
			}
			else
			{
				Set_Percent(InterpTo,Env,HConfig);
			}
		}
		break;
	case EDlFmJoystick_ControlSource::Handle0FollowHandle1:
		{
			Set_Percent(FMath::Vector2DInterpTo(
				CurVal, Env->Draft.AIHandle.CurVal, DeltaTime, Env->Config.Player.Speed_WhenFollowOtherHandle),Env,HConfig);
			if(HConfig.bBroadcastOnTick)
			{
				OnHandle0Changed.Broadcast(CurVal);
			}
		}
		break;
	case EDlFmJoystick_ControlSource::InputEvent_Mouse:
	case EDlFmJoystick_ControlSource::InputEvent_Touch:
		if(HConfig.bBroadcastOnTick)
		{
			OnHandle0Changed.Broadcast(CurVal);
		}
		break;
	default:
		break;
	}

}

void SDlFmJoystick::FDlFmJoystick_Draft::FHandleDraft::OnPaint(FSlateWindowElementList& ElementList, uint32 InLayer,
	const FGeometry& AllottedGeometry, const FDlFmJoystick_HandleConfig& HConfig) const
{
	HConfig.DrawBox.OnPaint(ElementList,InLayer,AllottedGeometry,ActualPosition-ActualSize/2,ActualSize);
}

void SDlFmJoystick::SetHandle0_Event(const FGeometry& InGeometry, const FPointerEvent& InEvent)
{
	const FVector2D LocalPos = InGeometry.AbsoluteToLocal(InEvent.GetScreenSpacePosition());
	const FVector2D WidgetPercent = (LocalPos-Draft.ReachableArea_Center) / Draft.ReachableArea_HalfSize;
	Draft.PlayerHandle.Set_Percent(WidgetPercent2ValuePercent_CheckReverse(WidgetPercent),this,Config.PlayerHandle);
}

void SDlFmJoystick::RefreshGeometry(const FGeometry& SelfGeometry)
{
	if (Draft.Cache_SelfGeometry != SelfGeometry)
	{
		Draft.Cache_SelfGeometry = SelfGeometry;
		const FVector2D TmpLocalSize = Draft.Cache_SelfGeometry.GetLocalSize();

		Config.ReachableArea.CheckSet(TmpLocalSize,Draft.ReachableArea_Center,Draft.ReachableArea_HalfSize);
		
		Draft.Cache_MaxRadius = FMath::Min(Draft.ReachableArea_HalfSize.X, Draft.ReachableArea_HalfSize.Y);

		Config.PlayerHandle.SetSize(Draft.ReachableArea_HalfSize,Draft.PlayerHandle.ActualSize);
		Config.AIHandle.SetSize(Draft.ReachableArea_HalfSize,Draft.AIHandle.ActualSize);
		
		SetHandlePosition(Draft.PlayerHandle.CurVal,Draft.PlayerHandle.ActualPosition);
		SetHandlePosition(Draft.AIHandle.CurVal,Draft.AIHandle.ActualPosition);
	}
}

void SDlFmJoystick::SetHandlePosition(const FVector2D& ValuePercent,FVector2D& ActualPosition) const
{
	const FVector2D WidgetPercent = ValuePercent2WidgetPercent_CheckReverse(ValuePercent);
	ActualPosition=Draft.ReachableArea_Center+WidgetPercent*Draft.ReachableArea_HalfSize;
}

END_SLATE_FUNCTION_BUILD_OPTIMIZATION
