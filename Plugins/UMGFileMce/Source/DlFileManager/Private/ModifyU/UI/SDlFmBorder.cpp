// Copyright(c) 2023 Dragon Li Software Limited. All Rights Reserved.

#include "ModifyU/UI/SDlFmBorder.h"
#include "Rendering/DrawElements.h"

static FName SDlFmBorderTypeName("SDlFmBorder");

SLATE_IMPLEMENT_WIDGET(SDlFmBorder)
void SDlFmBorder::PrivateRegisterAttributes(FSlateAttributeInitializer& AttributeInitializer)
{
	SLATE_ADD_MEMBER_ATTRIBUTE_DEFINITION_WITH_NAME(AttributeInitializer, "BorderImage", BorderImageAttribute, EInvalidateWidgetReason::Paint);
	SLATE_ADD_MEMBER_ATTRIBUTE_DEFINITION_WITH_NAME(AttributeInitializer, "BorderBackgroundColor", BorderBackgroundColorAttribute, EInvalidateWidgetReason::Paint);
	SLATE_ADD_MEMBER_ATTRIBUTE_DEFINITION_WITH_NAME(AttributeInitializer, "DesiredSizeScale", DesiredSizeScaleAttribute, EInvalidateWidgetReason::Layout);
	SLATE_ADD_MEMBER_ATTRIBUTE_DEFINITION_WITH_NAME(AttributeInitializer, "ShowDisabledEffect", ShowDisabledEffectAttribute, EInvalidateWidgetReason::Paint);
}

SDlFmBorder::SDlFmBorder()
	: BorderImageAttribute(*this, FCoreStyle::Get().GetBrush( "Border" ))
	, BorderBackgroundColorAttribute(*this, FLinearColor::White)
	, DesiredSizeScaleAttribute(*this, FVector2D(1,1))
	, ShowDisabledEffectAttribute(*this, true)
	, bFlipForRightToLeftFlowDirection(false)
{
}

SDlFmBorder::~SDlFmBorder() = default;

void SDlFmBorder::Construct( const SDlFmBorder::FArguments& InArgs )
{
	DlPaintFeature.Init(InArgs);//Dl+
	// Only do this if we're exactly an SDlFmBorder
	if ( GetType() == SDlFmBorderTypeName )
	{
		SetCanTick(false);
		bCanSupportFocus = false;
	}

	SetContentScale(InArgs._ContentScale);
	SetColorAndOpacity(InArgs._ColorAndOpacity);
	SetDesiredSizeScale(InArgs._DesiredSizeScale);
	SetShowEffectWhenDisabled(InArgs._ShowEffectWhenDisabled);

	bFlipForRightToLeftFlowDirection = InArgs._FlipForRightToLeftFlowDirection;

	SetBorderImage(InArgs._BorderImage);
	SetBorderBackgroundColor(InArgs._BorderBackgroundColor);
	SetForegroundColor(InArgs._ForegroundColor);

	if (InArgs._OnMouseButtonDown.IsBound())
	{
		SetOnMouseButtonDown(InArgs._OnMouseButtonDown);
	}

	if (InArgs._OnMouseButtonUp.IsBound())
	{
		SetOnMouseButtonUp(InArgs._OnMouseButtonUp);
	}

	if (InArgs._OnMouseMove.IsBound())
	{
		SetOnMouseMove(InArgs._OnMouseMove);
	}

	if (InArgs._OnMouseDoubleClick.IsBound())
	{
		SetOnMouseDoubleClick(InArgs._OnMouseDoubleClick);
	}

	ChildSlot
	.HAlign(InArgs._HAlign)
	.VAlign(InArgs._VAlign)
	.Padding(InArgs._Padding)
	[
		InArgs._Content.Widget
	];
}

void SDlFmBorder::SetContent( TSharedRef< SWidget > InContent )
{
	ChildSlot
	[
		InContent
	];
}

const TSharedRef< SWidget >& SDlFmBorder::GetContent() const
{
	return ChildSlot.GetWidget();
}

void SDlFmBorder::ClearContent()
{
	ChildSlot.DetachWidget();
}

int32 SDlFmBorder::OnPaint( const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled ) const
{
	const FSlateBrush* BrushResource = BorderImageAttribute.Get();
		
	const bool bEnabled = ShouldBeEnabled(bParentEnabled);
	
	DlPaintFeature.Refresh(AllottedGeometry);//Dl+

	if ( BrushResource && BrushResource->DrawAs != ESlateBrushDrawType::NoDrawType )
	{
		const bool bShowDisabledEffect = GetShowDisabledEffect();
		const ESlateDrawEffect DrawEffects = (bShowDisabledEffect && !bEnabled) ? ESlateDrawEffect::DisabledEffect : ESlateDrawEffect::None;

		if (bFlipForRightToLeftFlowDirection && GSlateFlowDirection == EFlowDirection::RightToLeft)
		{
			const FGeometry FlippedGeometry = AllottedGeometry.MakeChild(FSlateRenderTransform(FScale2D(-1, 1)));
			const FVector2D FlippedOffset=FVector2D(-DlPaintFeature.Cached_RealOffset.X,DlPaintFeature.Cached_RealOffset.Y);//Dl+
			FSlateDrawElement::MakeBox(
				OutDrawElements,
				LayerId,
				FlippedGeometry.ToPaintGeometry(DlPaintFeature.Cached_RealSize, FSlateLayoutTransform(1, FlippedOffset)),//Dl+
				//FlippedGeometry.ToPaintGeometry(FlippedOffset,DlPaintFeature.Cached_RealSize,1),//Dl+
				BrushResource,
				DrawEffects,
				BrushResource->GetTint(InWidgetStyle) * InWidgetStyle.GetColorAndOpacityTint() * BorderBackgroundColorAttribute.Get().GetColor(InWidgetStyle)
			);
		}
		else
		{
			FSlateDrawElement::MakeBox(
				OutDrawElements,
				LayerId,
				AllottedGeometry.ToPaintGeometry(DlPaintFeature.Cached_RealSize, FSlateLayoutTransform(1,  DlPaintFeature.Cached_RealOffset)),//Dl+
				//AllottedGeometry.ToPaintGeometry(DlPaintFeature.Cached_RealOffset,DlPaintFeature.Cached_RealSize,1),//Dl+
				BrushResource,
				DrawEffects,
				BrushResource->GetTint(InWidgetStyle) * InWidgetStyle.GetColorAndOpacityTint() * BorderBackgroundColorAttribute.Get().GetColor(InWidgetStyle)
			);
		}
	}

	return SCompoundWidget::OnPaint(Args, AllottedGeometry, MyCullingRect, OutDrawElements, LayerId, InWidgetStyle, bEnabled );
}

FVector2D SDlFmBorder::ComputeDesiredSize(float LayoutScaleMultiplier) const
{
	return DesiredSizeScaleAttribute.Get() * Super::ComputeDesiredSize(LayoutScaleMultiplier);
}

void SDlFmBorder::SetBorderBackgroundColor(TAttribute<FSlateColor> InColorAndOpacity)
{
	BorderBackgroundColorAttribute.Assign(*this, InColorAndOpacity);
}

void SDlFmBorder::SetDesiredSizeScale(TAttribute<FVector2D> InDesiredSizeScale)
{
	DesiredSizeScaleAttribute.Assign(*this, InDesiredSizeScale);
}

void SDlFmBorder::SetHAlign(EHorizontalAlignment HAlign)
{
	ChildSlot.SetHorizontalAlignment(HAlign);
}

void SDlFmBorder::SetVAlign(EVerticalAlignment VAlign)
{
	ChildSlot.SetVerticalAlignment(VAlign);
}

void SDlFmBorder::SetPadding(TAttribute<FMargin> InPadding)
{
	ChildSlot.SetPadding(MoveTemp(InPadding));
}

void SDlFmBorder::SetShowEffectWhenDisabled(TAttribute<bool> InShowEffectWhenDisabled)
{
	ShowDisabledEffectAttribute.Assign(*this, InShowEffectWhenDisabled);
}

void SDlFmBorder::SetBorderImage(TAttribute<const FSlateBrush*> InBorderImage)
{
	BorderImageAttribute.Assign(*this, InBorderImage);
}

void SDlFmBorder::FDlPaintFeature::Init(const FArguments& InArgs)//Dl+
{//Dl+
	Paint_OffsetPercent=InArgs._Paint_OffsetPercent;//Dl+
	Paint_OverflowPercent=InArgs._Paint_OverflowPercent;//Dl+
}//Dl+

void SDlFmBorder::FDlPaintFeature::Refresh(const FGeometry& AllottedGeometry)//Dl+
{//Dl+
	if(AllottedGeometry!=Cached_Geometry)//Dl+
		{//Dl+
		Cached_Geometry=AllottedGeometry;//Dl+
		Cached_RealSize = Cached_Geometry.GetLocalSize()*Paint_OverflowPercent;//Dl+
		Cached_RealOffset = Cached_Geometry.GetLocalSize()*(Paint_OffsetPercent-(Paint_OverflowPercent-1)/2);//Dl+
		}//Dl+
}//Dl+
