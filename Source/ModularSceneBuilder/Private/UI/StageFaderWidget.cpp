// Copyright Epic Games, Inc. All Rights Reserved.

#include "UI/StageFaderWidget.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/CheckBox.h"
#include "Components/Image.h"
#include "Components/SizeBox.h"
#include "Components/Slider.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageFaderWidget)

#define LOCTEXT_NAMESPACE "StageCraftFader"

namespace StageFader
{
	const FLinearColor StripBackground(0.018f, 0.018f, 0.022f, 1.f);
	const FLinearColor TextColor(0.82f, 0.82f, 0.85f, 1.f);
	const FLinearColor ChannelColors[3] = { FLinearColor(0.9f, 0.12f, 0.1f), FLinearColor(0.15f, 0.8f, 0.2f), FLinearColor(0.2f, 0.4f, 1.f) };

	void StyleText(UTextBlock& Text, int32 Size)
	{
		FSlateFontInfo Font = Text.GetFont();
		Font.Size = Size;
		Text.SetFont(Font);
		Text.SetColorAndOpacity(FSlateColor(TextColor));
		Text.SetJustification(ETextJustify::Center);
	}

	UVerticalBoxSlot* AddRow(UVerticalBox& Box, UWidget& Child, EHorizontalAlignment HAlign, const FMargin& Padding)
	{
		UVerticalBoxSlot* Slot = Box.AddChildToVerticalBox(&Child);
		Slot->SetHorizontalAlignment(HAlign);
		Slot->SetPadding(Padding);
		return Slot;
	}
}

void UStageFaderWidget::NativeOnInitialized()
{
	Super::NativeOnInitialized();

	// The whole strip takes the wheel, not just the thin slider bar.
	SetVisibility(ESlateVisibility::Visible);

	if (WidgetTree && !WidgetTree->RootWidget)
	{
		BuildDefaultTree();
	}
}

void UStageFaderWidget::BuildDefaultTree()
{
	using namespace StageFader;

	USizeBox* Root = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass(), TEXT("StripSize"));
	Root->SetWidthOverride(StripWidth);
	WidgetTree->RootWidget = Root;

	UBorder* Background = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("StripBackground"));
	Background->SetBrushColor(StripBackground);
	Background->SetPadding(FMargin(2.f, 2.f, 2.f, 4.f));
	Root->AddChild(Background);

	UVerticalBox* Column = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("StripColumn"));
	Background->SetContent(Column);

	GroupColorStrip = WidgetTree->ConstructWidget<UImage>(UImage::StaticClass(), TEXT("GroupColorStrip"));
	GroupColorStrip->SetDesiredSizeOverride(FVector2D(StripWidth, 3.0));
	AddRow(*Column, *GroupColorStrip, HAlign_Fill, FMargin(0.f, 0.f, 0.f, 3.f));

	NameText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("NameText"));
	StyleText(*NameText, 8);
	NameText->SetClipping(EWidgetClipping::ClipToBounds);
	AddRow(*Column, *NameText, HAlign_Fill, FMargin(0.f, 0.f, 0.f, 2.f));

	USizeBox* FaderBox = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass(), TEXT("FaderSize"));
	FaderBox->SetHeightOverride(FaderHeight);
	AddRow(*Column, *FaderBox, HAlign_Center, FMargin(0.f, 2.f));

	ValueSlider = WidgetTree->ConstructWidget<USlider>(USlider::StaticClass(), TEXT("ValueSlider"));
	ValueSlider->SetOrientation(Orient_Vertical);
	ValueSlider->SetSliderHandleColor(FLinearColor::White);
	FaderBox->AddChild(ValueSlider);

	ValueToggle = WidgetTree->ConstructWidget<UCheckBox>(UCheckBox::StaticClass(), TEXT("ValueToggle"));
	AddRow(*Column, *ValueToggle, HAlign_Center, FMargin(0.f, 2.f));

	ValueText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("ValueText"));
	StyleText(*ValueText, 8);
	AddRow(*Column, *ValueText, HAlign_Fill, FMargin(0.f, 2.f, 0.f, 0.f));
}

bool UStageFaderWidget::IsNumeric() const
{
	const EStageParameterType Type = Descriptor.GetType();
	return IsColorChannel() || ((Type == EStageParameterType::Float || Type == EStageParameterType::Integer) && Descriptor.HasRange());
}

void UStageFaderWidget::GetDisplayRange(double& OutMin, double& OutMax) const
{
	if (IsColorChannel())
	{
		OutMin = 0.0;
		OutMax = 100.0;
		return;
	}
	OutMin = Descriptor.Min * Descriptor.DisplayScale;
	OutMax = Descriptor.Max * Descriptor.DisplayScale;
	if (OutMin > OutMax)
	{
		Swap(OutMin, OutMax); // A negative display scale flips the range.
	}
}

void UStageFaderWidget::OnInitializeControl()
{
	using namespace StageFader;

	const bool bEditable = !Descriptor.bReadOnly;
	const bool bToggle = Descriptor.GetType() == EStageParameterType::Bool;
	const bool bNumeric = IsNumeric();
	const FLinearColor BarColor = IsColorChannel() ? ChannelColors[FMath::Clamp(ColorComponent, 0, 2)] : GroupColor;

	if (NameText)
	{
		static const FText ChannelNames[3] = { LOCTEXT("Red", "Red"), LOCTEXT("Green", "Green"), LOCTEXT("Blue", "Blue") };
		NameText->SetText(IsColorChannel() ? ChannelNames[FMath::Clamp(ColorComponent, 0, 2)] : Descriptor.DisplayName);
	}
	if (GroupColorStrip)
	{
		GroupColorStrip->SetColorAndOpacity(BarColor);
	}

	if (ValueSlider)
	{
		ValueSlider->SetVisibility(bNumeric ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
		if (bNumeric)
		{
			double Min = 0.0, Max = 1.0;
			GetDisplayRange(Min, Max);
			ValueSlider->SetMinValue(static_cast<float>(Min));
			ValueSlider->SetMaxValue(static_cast<float>(Max));
			ValueSlider->SetStepSize(Descriptor.GetType() == EStageParameterType::Integer ? 1.f : 0.f);
			ValueSlider->SetSliderBarColor(BarColor);
			ValueSlider->SetIsEnabled(bEditable);
			ValueSlider->OnValueChanged.AddUniqueDynamic(this, &ThisClass::HandleSliderChanged);
			ValueSlider->OnMouseCaptureEnd.AddUniqueDynamic(this, &ThisClass::HandleSliderCaptureEnd);
		}
	}

	if (ValueToggle)
	{
		ValueToggle->SetVisibility(bToggle ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
		if (bToggle)
		{
			ValueToggle->SetIsEnabled(bEditable);
			ValueToggle->OnCheckStateChanged.AddUniqueDynamic(this, &ThisClass::HandleToggleChanged);
		}
	}
}

double UStageFaderWidget::ReadDisplayValue(const FStageParameterValue& InValue) const
{
	if (IsColorChannel())
	{
		const FLinearColor& Color = InValue.Color;
		return (ColorComponent == 0 ? Color.R : (ColorComponent == 1 ? Color.G : Color.B)) * 100.0;
	}
	return ToDisplay(InValue);
}

void UStageFaderWidget::ApplyValue(const FStageParameterValue& InValue, bool bForce)
{
	if (ValueSlider && IsNumeric() && (bForce || !IsUserInteracting(ValueSlider)))
	{
		ValueSlider->SetValue(static_cast<float>(ReadDisplayValue(InValue)));
	}
	if (ValueToggle && InValue.Type == EStageParameterType::Bool)
	{
		ValueToggle->SetIsChecked(InValue.bBool);
	}
	if (ValueText)
	{
		if (IsColorChannel())
		{
			FNumberFormattingOptions Options;
			Options.MaximumFractionalDigits = 0;
			ValueText->SetText(FText::Format(LOCTEXT("ChannelPercent", "{0} %"), FText::AsNumber(ReadDisplayValue(InValue), &Options)));
		}
		else
		{
			ValueText->SetText(FormatValue(Descriptor, InValue));
		}
	}
}

void UStageFaderWidget::CommitDisplayValue(double DisplayValue)
{
	if (IsColorChannel())
	{
		FLinearColor Color = Descriptor.Value.Color;
		const float Channel = static_cast<float>(FMath::Clamp(DisplayValue / 100.0, 0.0, 1.0));
		(ColorComponent == 0 ? Color.R : (ColorComponent == 1 ? Color.G : Color.B)) = Channel;
		CommitValue(FStageParameterValue::MakeColor(Color));
		return;
	}
	CommitValue(FromDisplay(DisplayValue));
}

void UStageFaderWidget::HandleSliderChanged(float InValue)
{
	CommitDisplayValue(InValue);
}

void UStageFaderWidget::HandleSliderCaptureEnd()
{
	FinishInteraction();
}

void UStageFaderWidget::HandleToggleChanged(bool bIsChecked)
{
	CommitValue(FStageParameterValue::MakeBool(bIsChecked));
}

FReply UStageFaderWidget::NativeOnMouseWheel(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent)
{
	if (!IsNumeric() || Descriptor.bReadOnly)
	{
		return Super::NativeOnMouseWheel(InGeometry, InMouseEvent);
	}

	double Min = 0.0, Max = 1.0;
	GetDisplayRange(Min, Max);
	double Step = (Max - Min) * (InMouseEvent.IsShiftDown() ? 0.001 : 0.01);
	if (Descriptor.GetType() == EStageParameterType::Integer)
	{
		Step = FMath::Max(1.0, FMath::RoundToDouble(Step));
	}

	const double Current = ReadDisplayValue(Descriptor.Value);
	CommitDisplayValue(FMath::Clamp(Current + Step * InMouseEvent.GetWheelDelta(), Min, Max));
	// Handled: the wheel over a strip must not also change camera fly speed.
	return FReply::Handled();
}

#undef LOCTEXT_NAMESPACE
