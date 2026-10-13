// Copyright(c) 2023 Dragon Li Software Limited. All Rights Reserved.

#include "UI/Fragment/DlFmJoystick_Lib.h"


void FDlFmJoystick_DrawBox::OnPaint(FSlateWindowElementList& ElementList, uint32 InLayer,
	const FGeometry& AllottedGeometry) const
{
	
	if(bVisible)
	{
		const FVector2D RealSize=AllottedGeometry.GetLocalSize()*Paint_OverflowPercent;
		const FVector2D RealOffset=AllottedGeometry.GetLocalSize()*(Paint_OffsetPercent-(Paint_OverflowPercent-1)/2);
		FSlateDrawElement::MakeBox(
			ElementList,
			InLayer,
			AllottedGeometry.ToPaintGeometry(RealSize, FSlateLayoutTransform(1, RealOffset)),
			//AllottedGeometry.ToPaintGeometry(RealOffset,RealSize,1),
			&Icon,
			ESlateDrawEffect::None,
			IconColor);
	}
}

void FDlFmJoystick_DrawBox::OnPaint(FSlateWindowElementList& ElementList, uint32 InLayer,
                                    const FGeometry& AllottedGeometry, const FVector2D& LocalOffset,
                                    const FVector2D& LocalSize) const
{
	if(bVisible)
	{
		const FVector2D RealSize=LocalSize*Paint_OverflowPercent;
		const FVector2D RealOffset=LocalOffset+LocalSize*(Paint_OffsetPercent-(Paint_OverflowPercent-1)/2);
		FSlateDrawElement::MakeBox(
			ElementList,
			InLayer,
			AllottedGeometry.ToPaintGeometry(RealSize, FSlateLayoutTransform(1, RealOffset)),
			//AllottedGeometry.ToPaintGeometry(RealOffset,RealSize,1),
			&Icon,
			ESlateDrawEffect::None,
			IconColor);
	}
}

void FDlFmJoystick_ReachableAreaConfig::CheckSet(const FVector2D& WidgetSize,FVector2D &LocalCenter,FVector2D& ReachableHalfSize)
{
	if(bMarginByPercent)
	{
		MarginByPercent.Left=FMath::Max(0.f,MarginByPercent.Left);
		MarginByPercent.Right=FMath::Max(0.f,MarginByPercent.Right);
		MarginByPercent.Top=FMath::Max(0.f,MarginByPercent.Top);
		MarginByPercent.Bottom=FMath::Max(0.f,MarginByPercent.Bottom);
		const float TmpVal1=(MarginByPercent.Left+MarginByPercent.Right-0.99)/2;
		if(TmpVal1>0)
		{
			MarginByPercent.Left-=TmpVal1;
			MarginByPercent.Right-=TmpVal1;
		}
		const float TmpVal2=(MarginByPercent.Top+MarginByPercent.Bottom-0.99)/2;
		if(TmpVal2>0)
		{
			MarginByPercent.Top-=TmpVal1;
			MarginByPercent.Bottom-=TmpVal1;
		}
		
		ReachableHalfSize.X=(1-MarginByPercent.Left-MarginByPercent.Right)/2*WidgetSize.X;
		ReachableHalfSize.Y=(1-MarginByPercent.Top-MarginByPercent.Bottom)/2*WidgetSize.Y;
		LocalCenter.X=WidgetSize.X*MarginByPercent.Left+ReachableHalfSize.X;
		LocalCenter.Y=WidgetSize.Y*MarginByPercent.Top+ReachableHalfSize.Y;
	}
	else
	{
		MarginByValue.Left=FMath::Max(0.f,MarginByValue.Left);
		MarginByValue.Right=FMath::Max(0.f,MarginByValue.Right);
		MarginByValue.Top=FMath::Max(0.f,MarginByValue.Top);
		MarginByValue.Bottom=FMath::Max(0.f,MarginByValue.Bottom);
		const float TmpVal1=(MarginByValue.Left+MarginByValue.Right-WidgetSize.X+1)/2;
		if(TmpVal1>0)
		{
			MarginByValue.Left-=TmpVal1;
			MarginByValue.Right-=TmpVal1;
		}
		const float TmpVal2=(MarginByValue.Top+MarginByValue.Bottom-WidgetSize.Y+1)/2;
		if(TmpVal2>0)
		{
			MarginByValue.Top-=TmpVal1;
			MarginByValue.Bottom-=TmpVal1;
		}
		
		ReachableHalfSize.X=(WidgetSize.X-MarginByValue.Left-MarginByValue.Right)/2;
		ReachableHalfSize.Y=(WidgetSize.Y-MarginByValue.Top-MarginByValue.Bottom)/2;
		LocalCenter.X=MarginByValue.Left+ReachableHalfSize.X;
		LocalCenter.Y=MarginByValue.Top+ReachableHalfSize.Y;
	}
}

FVector2D FDlFmJoystick_ReachableAreaConfig::ClampPercent(const FVector2D& Src) const
{
	switch (Type)
	{
	case EDlFmJoystick_ReachableAreaType::Rectangle:
		{
			return FVector2D(FMath::Clamp(Src.X, -1.f, 1.f),
							 FMath::Clamp(Src.Y, -1.f, 1.f));
		}
	case EDlFmJoystick_ReachableAreaType::Circle:
		{
			const float LocalPercentSize = Src.SizeSquared(); //Square of 1 is 1, so optimize Sqrt operation here.
			if (LocalPercentSize > 1)
			{
				return Src / LocalPercentSize;
			}
			else
			{
				return Src;
			}
		}
	case EDlFmJoystick_ReachableAreaType::Horizontal:
		{
			return FVector2D(FMath::Clamp(Src.X, -1.f, 1.f),0);
		}
	case EDlFmJoystick_ReachableAreaType::Vertical:
		{
			return FVector2D(0, FMath::Clamp(Src.X, -1.f, 1.f));
		}
	case EDlFmJoystick_ReachableAreaType::None:
		return Src;
	default:
		return FVector2D::ZeroVector;
	}
}

void FDlFmJoystick_HandleConfig::SetSize(const FVector2D& ReachableAreaHalfSize, FVector2D& ActualSize) const
{
	if (bIconSizeByPercent)
	{
		ActualSize = ReachableAreaHalfSize * SizePercent_IconToReachableArea * 2;
	}
	else
	{
		ActualSize=SizeValue_Inch;
	}
}






